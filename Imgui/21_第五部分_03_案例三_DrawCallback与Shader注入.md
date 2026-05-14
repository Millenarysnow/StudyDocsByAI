# 第 5.3 章 · 案例三：拦截渲染指令 —— `UserCallback` 与 Shader 参数注入

> **本章目标**：把 ImGui 与你自家 GPU 渲染管线的"接缝"打通——读完后你应该能：(1) 把任何自定义 shader 输出嵌入 ImGui 窗口；(2) 把游戏 3D 场景作为 ImGui::Image 显示；(3) 用 dissolve / 扫光等 shader 特效画自定义控件；(4) 在 ImGui 渲染中途切换 GPU 状态而不污染后续控件。
>
> **本章对应源码**：第 3.4 章（`AddCallback` / `ImDrawCallback_ResetRenderState`）、`imgui.h:3158~3173`（`ImDrawCallback` 类型 + `ImDrawCallback_ResetRenderState`）、`backends/imgui_impl_opengl3.cpp:RenderDrawData` 内对 `pcmd->UserCallback` 的处理、`imgui.h:367~381`（`ImTextureRef`）。
>
> **前置阅读**：第 3.4 章（顶点合并 / Backend 契约）、第 0.4 章（GPU 管线 + Alpha 混合）。

---

## 5.3.0 三种"自定义渲染"场景

ImGui 与自家 GPU 代码的协作，按"嵌入度"分三种：

```
┌──────────────────────────────────────────────────────────────────┐
│ 难度 1：作为纹理嵌入                                                │
│   把场景渲染到 RTT → 作为 Texture → ImGui::Image() 显示             │
│   - 最简单                                                       │
│   - ImGui 渲染时只是采样这个纹理                                    │
│   - 适合：场景视图、Material 预览                                   │
│                                                                  │
├──────────────────────────────────────────────────────────────────┤
│ 难度 2：自定义 shader 替代 ImGui 默认 shader                         │
│   控件几何由 ImGui 生成（PrimReserve + 顶点写入）                    │
│   AddCallback 切换到自家 shader 渲染这部分顶点                      │
│   再 AddCallback(ResetRenderState) 切回                          │
│   - 中等难度                                                     │
│   - 适合：dissolve 按钮、扫光进度条、彩虹文字                       │
│                                                                  │
├──────────────────────────────────────────────────────────────────┤
│ 难度 3：在 ImGui cmd 中间嵌入完整 3D 渲染                            │
│   AddCallback 内部直接调你的 3D 渲染管线                           │
│   - 复杂（涉及状态保存/恢复、深度缓冲）                             │
│   - 适合：在小窗口里画 3D gizmo、IK 骨骼、几何工具                  │
└──────────────────────────────────────────────────────────────────┘
```

本章三个实战分别覆盖这三个层次。

---

## 5.3.1 `ImDrawCallback` 回顾

```cpp
// imgui.h:3166
typedef void (*ImDrawCallback)(const ImDrawList* parent_list, const ImDrawCmd* cmd);

// imgui.h:3173
#define ImDrawCallback_ResetRenderState   (ImDrawCallback)(-8)
```

回看第 3.4 章给的 `AddCallback` 内部：

```cpp
void ImDrawList::AddCallback(ImDrawCallback callback, void* userdata, size_t userdata_size)
{
    ImDrawCmd* curr_cmd = &CmdBuffer.Data[CmdBuffer.Size - 1];
    if (curr_cmd->ElemCount != 0) {
        AddDrawCmd();
        curr_cmd = &CmdBuffer.Data[CmdBuffer.Size - 1];
    }
    
    curr_cmd->UserCallback = callback;
    if (userdata_size == 0) {
        curr_cmd->UserCallbackData = userdata;
    } else {
        curr_cmd->UserCallbackData = NULL;
        curr_cmd->UserCallbackDataSize = (int)userdata_size;
        curr_cmd->UserCallbackDataOffset = _CallbacksDataBuf.Size;
        _CallbacksDataBuf.resize(_CallbacksDataBuf.Size + (int)userdata_size);
        memcpy(_CallbacksDataBuf.Data + curr_cmd->UserCallbackDataOffset, userdata, userdata_size);
    }
    
    AddDrawCmd();   // 强制空 cmd 跟在 callback 后
}
```

**关键点**：

1. `AddCallback` 后**强制 `AddDrawCmd()`** —— callback cmd 之后会有一个新空 cmd 接收后续几何。
2. `userdata_size == 0`：直接存指针（你保证生命周期）。
3. `userdata_size > 0`：复制数据到 `_CallbacksDataBuf`，cmd 存 offset。

---

## 5.3.2 Backend 端处理 callback

打开 `backends/imgui_impl_opengl3.cpp` 中的 `RenderDrawData`（第 0.4 章已读）：

```cpp
for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
{
    const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];
    if (pcmd->UserCallback != NULL)
    {
        if (pcmd->UserCallback == ImDrawCallback_ResetRenderState) {
            ImGui_ImplOpenGL3_SetupRenderState(...);    // 重置状态
        } else {
            pcmd->UserCallback(draw_list, pcmd);        // 调用 callback
        }
    }
    else
    {
        // 普通渲染分支
        glScissor(...);
        glBindTexture(...);
        glDrawElements(...);
    }
}
```

**3 个分支**：

1. `UserCallback == ResetRenderState`：backend 自己重新 setup 状态。
2. `UserCallback != NULL`：调用用户函数。
3. `UserCallback == NULL`：正常画三角形。

---

## 5.3.3 实战 1：把 3D 场景作为 ImGui::Image 显示（难度 1）

**目标**：编辑器场景视图——把 Hazel 引擎渲染的 3D 世界显示在 ImGui 窗口中。

### 渲染目标（RTT / FBO）

每帧：
1. 把场景渲染到 framebuffer（"render-to-texture"）。
2. 取得 framebuffer 颜色附件的纹理 ID。
3. `ImGui::Image(tex_id, viewport_size)` 把它显示。

### Hazel 框架代码

```cpp
// EditorLayer.cpp
class EditorLayer : public Layer {
public:
    void OnAttach() override {
        // 创建 framebuffer
        Hazel::FramebufferSpecification fb_spec;
        fb_spec.Attachments = { 
            Hazel::FramebufferTextureFormat::RGBA8,
            Hazel::FramebufferTextureFormat::Depth
        };
        fb_spec.Width = 1280;
        fb_spec.Height = 720;
        m_Framebuffer = Hazel::Framebuffer::Create(fb_spec);
    }
    
    void OnUpdate(Hazel::Timestep ts) override {
        // ===== 渲染场景到 framebuffer =====
        m_Framebuffer->Bind();
        Hazel::RenderCommand::Clear();
        m_Scene->OnRender();   // 你的 3D 渲染
        m_Framebuffer->Unbind();
    }
    
    void OnImGuiRender() override {
        ImGui::Begin("Viewport");
        
        ImVec2 viewport_size = ImGui::GetContentRegionAvail();
        
        // 检查 viewport 大小变化
        if (m_ViewportSize != *((glm::vec2*)&viewport_size)) {
            m_ViewportSize = { viewport_size.x, viewport_size.y };
            m_Framebuffer->Resize((uint32_t)viewport_size.x, (uint32_t)viewport_size.y);
        }
        
        uint32_t tex_id = m_Framebuffer->GetColorAttachmentRendererID();
        
        // 关键：ImGui::Image 接 OpenGL 纹理 ID
        // 注意 OpenGL 纹理坐标 Y 倒置
        ImGui::Image((ImTextureID)(uintptr_t)tex_id,
                     viewport_size,
                     ImVec2(0, 1),    // uv0 - 左上（OpenGL 是左下）
                     ImVec2(1, 0));   // uv1 - 右下
        
        ImGui::End();
    }
};
```

### 关键细节 1：UV Y-Flip

OpenGL 纹理的 V 坐标**底部为 0、顶部为 1**。但 ImGui::Image 默认 uv0=(0,0) 在左上、uv1=(1,1) 在右下。

不翻转 → 场景上下颠倒。

```cpp
// OpenGL backend：传 uv0=(0,1), uv1=(1,0) 翻转
ImGui::Image(tex_id, size, ImVec2(0, 1), ImVec2(1, 0));

// DirectX / Vulkan 默认就是顶部为 0：
// ImGui::Image(tex_id, size);   // 不需要翻转
```

### 关键细节 2：FBO 大小变化

```cpp
if (m_ViewportSize != viewport_size) {
    m_ViewportSize = viewport_size;
    m_Framebuffer->Resize(...);
}
```

——用户拖动窗口调整大小时，FBO 也要相应调整——否则要么内容拉伸，要么内容只占一角。

### 关键细节 3：光标传递

ImGui::Image 不会让其下方的代码自动接收鼠标事件——你的 3D 场景需要鼠标交互（相机拖动 / 选中物体）：

```cpp
ImGui::Image(tex_id, viewport_size, ImVec2(0,1), ImVec2(1,0));

m_ViewportFocused = ImGui::IsItemFocused();
m_ViewportHovered = ImGui::IsItemHovered();

// 把 viewport 的鼠标位置算成"viewport 内相对坐标"
ImVec2 viewport_min = ImGui::GetItemRectMin();
ImVec2 mouse_in_viewport = ImGui::GetIO().MousePos - viewport_min;
m_Scene->OnMouseMove(mouse_in_viewport.x, mouse_in_viewport.y);

// ImGuiLayer 知道 viewport focus 后才把事件传给场景
```

---

## 5.3.4 实战 2：自定义 Shader 替代 ImGui 默认（难度 2）

**目标**：实现一个"扫光按钮"——按下时有渐变高光从左扫到右。

### 思路

1. 用 ImGui 的 `PrimReserve + PrimWriteVtx` 生成按钮的几何（4 顶点）。
2. `AddCallback` 切换到我们的"扫光 shader"。
3. 渲染——shader 里根据时间 + 顶点位置计算扫光颜色。
4. `AddCallback(ResetRenderState)` 切回 ImGui 默认 shader。

### Shader

```glsl
// SweepHighlight.vert
#version 410 core
layout(location = 0) in vec2 Pos;
layout(location = 1) in vec2 UV;
layout(location = 2) in vec4 Color;

uniform mat4 ProjMtx;
out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = UV;
    vColor = Color;
    gl_Position = ProjMtx * vec4(Pos, 0, 1);
}

// SweepHighlight.frag
#version 410 core
in vec2 vUV;
in vec4 vColor;
uniform float Time;
out vec4 OutColor;

void main() {
    float sweep = mod(Time * 0.5, 2.0);   // 扫光位置 0~2
    float dist = abs(vUV.x - (sweep - 0.5));
    float highlight = 1.0 - smoothstep(0.0, 0.3, dist);
    
    vec3 base = vColor.rgb;
    vec3 final = mix(base, vec3(1.0), highlight * 0.5);
    OutColor = vec4(final, vColor.a);
}
```

### Hazel 集成代码

```cpp
class SweepShaderManager {
public:
    void Init() {
        m_Shader = Hazel::Shader::Create("SweepHighlight",
                                         vert_src, frag_src);
    }
    
    static void RenderCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd) {
        SweepShaderManager* mgr = (SweepShaderManager*)cmd->UserCallbackData;
        
        // 取当前 ImGui 的投影矩阵（backend 已经设置）
        // 用 GL 直接 query：
        GLuint old_program = ...;
        glGetIntegerv(GL_CURRENT_PROGRAM, (GLint*)&old_program);
        
        // 切到我们的 shader
        glUseProgram(mgr->m_Shader->GetGLHandle());
        
        // 上传当前时间
        glUniform1f(mgr->m_TimeLoc, (float)ImGui::GetTime());
        
        // 注意：ProjMtx / Texture 等 uniform 都需要重新设
        // 这里假设我们的 shader 期望相同的 attribute 布局，所以 vertex layout 不变
        // 但 uniform 必须重设：
        // 取 ImGui backend 之前设置的 ProjMtx——通过我们自己缓存的全局
        glUniformMatrix4fv(mgr->m_ProjLoc, 1, GL_FALSE, &g_ImGuiProjMtx[0][0]);
        glUniform1i(mgr->m_TexLoc, 0);
        
        // 接下来 ImGui backend 会调 glDrawElements 用我们的 shader 画
        // 注意：callback 之后的 cmd 已经是新 cmd（AddDrawCmd）了
    }
    
    Hazel::Shader* m_Shader;
    GLint m_TimeLoc, m_ProjLoc, m_TexLoc;
};
```

### 用户代码

```cpp
bool SweepButton(const char* label) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    
    // 三段式
    ImGuiID id = window->GetID(label);
    ImVec2 label_size = ImGui::CalcTextSize(label);
    ImVec2 size(label_size.x + 16, label_size.y + 8);
    ImVec2 pos = window->DC.CursorPos;
    ImRect bb(pos, pos + size);
    
    ImGui::ItemSize(size);
    if (!ImGui::ItemAdd(bb, id)) return false;
    
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    
    ImDrawList* dl = window->DrawList;
    
    // 切到自定义 shader
    dl->AddCallback(SweepShaderManager::RenderCallback, &g_SweepMgr);
    
    // 用 ImGui 的 PrimReserve 生成 4 顶点
    ImU32 col = held ? IM_COL32(100, 100, 200, 255) : IM_COL32(80, 80, 180, 255);
    dl->AddRectFilled(bb.Min, bb.Max, col, 4.0f);
    
    // 切回 ImGui 默认 shader
    dl->AddCallback(ImDrawCallback_ResetRenderState, NULL);
    
    // 渲染 label（用 ImGui 默认）
    ImVec2 text_pos = bb.Min + ImVec2(8, 4);
    dl->AddText(text_pos, IM_COL32_WHITE, label);
    
    return pressed;
}
```

### 渲染顺序（cmd 序列）

```
... 之前的 cmd ...
[Cmd N+0]   普通 cmd（之前的内容）
[Cmd N+1]   UserCallback = SweepCallback
[Cmd N+2]   普通 cmd（含 sweep button 的 4 顶点 6 索引）—— 用 sweep shader 画
[Cmd N+3]   UserCallback = ResetRenderState
[Cmd N+4]   普通 cmd（含 button label）—— 用 ImGui 默认 shader
... 后续 cmd ...
```

### 性能含义

每个 `SweepButton` 增加 2 个 callback cmd + 1 个 reset cmd → 总共 **3 个额外 draw call**。

如果你的页面有 100 个 sweep button，那是 300 额外 draw call——CPU 端 driver overhead 可能受影响。

**优化**：把所有 sweep buttons **批量提交**，只切一次 shader：

```cpp
void BeginSweepBatch() {
    ImGui::GetWindowDrawList()->AddCallback(SweepShaderManager::RenderCallback, ...);
}
void EndSweepBatch() {
    ImGui::GetWindowDrawList()->AddCallback(ImDrawCallback_ResetRenderState, NULL);
}

// 用户：
BeginSweepBatch();
SweepButton("A");   // 此时 button 也用 sweep shader
SweepButton("B");
SweepButton("C");
EndSweepBatch();
```

---

## 5.3.5 实战 3：嵌入波形 / 直方图（难度 2）

**目标**：在 ImGui 窗口里显示音频波形或频谱直方图——几千个数据点用一个 draw call 渲染。

### 思路

1. 用 `PrimReserve` 自己生成顶点（直接 push 数据点）。
2. AddCallback 切到"波形 shader"——shader 里读顶点 attribute 渲染。
3. 切回。

### Vertex 结构

复用 ImDrawVert 的 pos/uv/col：

- `pos` = (x_index, y_value)
- `uv` = (sample_index_normalized, 0)
- `col` = waveform color

### Shader

```glsl
// Waveform.frag
#version 410 core
in vec2 vUV;
in vec4 vColor;
out vec4 OutColor;

void main() {
    // vUV.x = 0..1 横向位置
    float intensity = vUV.y;   // 用 uv.y 作 amplitude
    OutColor = vColor * vec4(1.0, intensity, intensity * 0.5, 1.0);
}
```

### 用户代码

```cpp
void DrawWaveform(const float* samples, int sample_count, ImVec2 size, ImU32 col) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    
    ImVec2 pos = window->DC.CursorPos;
    ImRect bb(pos, pos + size);
    
    ImGui::ItemSize(size);
    if (!ImGui::ItemAdd(bb, 0)) return;
    
    ImDrawList* dl = window->DrawList;
    
    // 切 shader
    dl->AddCallback(WaveformShaderManager::RenderCallback, &g_WaveMgr);
    
    // 生成几何（线条）
    int idx_count = (sample_count - 1) * 6;
    int vtx_count = sample_count * 2;
    dl->PrimReserve(idx_count, vtx_count);
    
    for (int i = 0; i < sample_count; i++) {
        float x = pos.x + (float)i / (sample_count - 1) * size.x;
        float center_y = pos.y + size.y * 0.5f;
        float amp_y = samples[i] * size.y * 0.4f;
        
        // 上下两个顶点（线条厚度 2 像素）
        dl->_VtxWritePtr[0].pos = ImVec2(x, center_y - amp_y);
        dl->_VtxWritePtr[0].uv  = ImVec2((float)i / (sample_count - 1), samples[i]);
        dl->_VtxWritePtr[0].col = col;
        
        dl->_VtxWritePtr[1].pos = ImVec2(x, center_y + amp_y);
        dl->_VtxWritePtr[1].uv  = ImVec2((float)i / (sample_count - 1), samples[i]);
        dl->_VtxWritePtr[1].col = col;
        
        dl->_VtxWritePtr += 2;
        
        // 索引（从前一顶点对到本顶点对的两个三角形）
        if (i > 0) {
            ImDrawIdx idx = (ImDrawIdx)(dl->_VtxCurrentIdx + i * 2);
            dl->_IdxWritePtr[0] = idx - 2;       // 左上
            dl->_IdxWritePtr[1] = idx - 1;       // 左下
            dl->_IdxWritePtr[2] = idx + 0;       // 右上
            dl->_IdxWritePtr[3] = idx + 0;       // 右上
            dl->_IdxWritePtr[4] = idx - 1;       // 左下
            dl->_IdxWritePtr[5] = idx + 1;       // 右下
            dl->_IdxWritePtr += 6;
        }
    }
    dl->_VtxCurrentIdx += vtx_count;
    
    dl->AddCallback(ImDrawCallback_ResetRenderState, NULL);
}
```

——一个 callback + N 个顶点 + 一个 reset = 3 cmd。1024 采样点的波形也只需要 3 draw call。

---

## 5.3.6 嵌入完整 3D 渲染（难度 3）

**目标**：在 ImGui::Image 区域内**实时**渲染 3D gizmo（不是先 RTT 再贴图）。

### 为什么不用 RTT

RTT 方案需要：
1. 每帧 bind FBO。
2. clear FBO。
3. 渲染场景到 FBO。
4. unbind FBO。
5. ImGui::Image 显示 FBO 颜色附件。

每个 viewport 一次 FBO 切换 + 清屏。如果你有 5 个小 viewport（动画预览/材质球/IK/骨骼/曲线），就是 5 次 FBO 切换——一些移动 GPU 上是显著开销（每次 FBO 切换 0.5 ms）。

**用 callback 直接渲染**：在 ImGui 主 framebuffer 上**直接画 3D**，scissor 限制到 widget 区域。

### 实现

```cpp
struct GizmoData {
    glm::mat4 ViewProj;
    Hazel::Mesh* Mesh;
    glm::mat4 ModelMatrix;
    ImVec2 ViewportMin, ViewportMax;   // 屏幕坐标
};

void GizmoRenderCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd) {
    GizmoData* data = (GizmoData*)cmd->UserCallbackData;
    
    // 1. 备份当前状态
    GLint backup_program;
    glGetIntegerv(GL_CURRENT_PROGRAM, &backup_program);
    GLboolean backup_depth = glIsEnabled(GL_DEPTH_TEST);
    GLint backup_scissor[4];
    glGetIntegerv(GL_SCISSOR_BOX, backup_scissor);
    
    // 2. 设置 3D 渲染状态
    glEnable(GL_DEPTH_TEST);   // 3D 需要深度测试
    glClear(GL_DEPTH_BUFFER_BIT);   // 清 viewport 区域的深度
    
    // 3. 限制渲染到 widget 区域（用 scissor，已经被 ImGui 设过了）
    // 但是 ImGui 的 ClipRect 是 backend 在 cmd 切换时设的——
    // callback cmd 自己的 ClipRect 是上一个 cmd 的 ClipRect
    // 所以这里 scissor 已经正确了
    
    // 4. 设 viewport（GL viewport != ImGui viewport）
    int fb_height = /* 取 framebuffer 高度 */;
    glViewport((int)data->ViewportMin.x,
               fb_height - (int)data->ViewportMax.y,   // GL Y-flip
               (int)(data->ViewportMax.x - data->ViewportMin.x),
               (int)(data->ViewportMax.y - data->ViewportMin.y));
    
    // 5. 绑定 3D shader + 上传 uniform
    g_GizmoShader->Bind();
    g_GizmoShader->SetMat4("u_ViewProj", data->ViewProj);
    g_GizmoShader->SetMat4("u_Model", data->ModelMatrix);
    
    // 6. 绑定 mesh + 渲染
    data->Mesh->Bind();
    glDrawElements(GL_TRIANGLES, data->Mesh->IndexCount, GL_UNSIGNED_INT, 0);
    
    // 7. 还原状态（让后续 ImGui cmd 正常工作）
    glViewport(0, 0, fb_width, fb_height);   // 恢复 viewport
    if (!backup_depth) glDisable(GL_DEPTH_TEST);
    glScissor(backup_scissor[0], backup_scissor[1], backup_scissor[2], backup_scissor[3]);
    glUseProgram(backup_program);
}

void DrawMeshPreview(Mesh* mesh, ImVec2 size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    
    ImVec2 pos = window->DC.CursorPos;
    ImRect bb(pos, pos + size);
    ImGui::ItemSize(size);
    if (!ImGui::ItemAdd(bb, 0)) return;
    
    // 准备数据
    GizmoData* data = (GizmoData*)IM_ALLOC(sizeof(GizmoData));   // ImGui 内存池
    data->Mesh = mesh;
    data->ModelMatrix = glm::rotate(glm::mat4(1), (float)ImGui::GetTime(), glm::vec3(0, 1, 0));
    data->ViewProj = /* 计算 */;
    data->ViewportMin = bb.Min;
    data->ViewportMax = bb.Max;
    
    // 注意：data 必须在 callback 调用前都活着——所以 IM_ALLOC + ImGui 帧内有效
    // 或者用 _CallbacksDataBuf 复制：
    ImDrawList* dl = window->DrawList;
    dl->AddCallback(GizmoRenderCallback, data, sizeof(GizmoData));   // 复制 sizeof 字节
    // sizeof > 0 模式：ImGui 把数据复制到 _CallbacksDataBuf，回调时通过 cmd->UserCallbackData 拿
    // 此处 data 局部变量可以释放——已被复制
    IM_FREE(data);
    
    // 不需要 ResetRenderState——因为 callback 内部已经手动还原
    // 但更稳妥：还是加一下
    dl->AddCallback(ImDrawCallback_ResetRenderState, NULL);
}
```

### 关键细节

#### 1. 状态备份

callback 里**必须**备份所有你要改的状态——否则改了状态后 ImGui 后续 cmd 会渲染异常（颜色错、深度异常、scissor 错）。

最稳妥的备份模板：

```cpp
// 备份
GLint backup_program;     glGetIntegerv(GL_CURRENT_PROGRAM, &backup_program);
GLuint backup_vao;        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, (GLint*)&backup_vao);
GLuint backup_vbo;        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, (GLint*)&backup_vbo);
GLboolean backup_depth = glIsEnabled(GL_DEPTH_TEST);
GLboolean backup_cull  = glIsEnabled(GL_CULL_FACE);
GLint backup_blend_src; glGetIntegerv(GL_BLEND_SRC_ALPHA, &backup_blend_src);
// ... 几十个状态 ...

// 你的渲染

// 还原
glUseProgram(backup_program);
glBindVertexArray(backup_vao);
// ...
```

——非常啰嗦。**更简单的方案**：直接用 `ImDrawCallback_ResetRenderState` 让 backend 自己重设：

```cpp
dl->AddCallback(MyRenderCallback, &data);
dl->AddCallback(ImDrawCallback_ResetRenderState, NULL);
```

`ResetRenderState` 的 backend 实现就是把"ImGui 期望的状态"全部重新设一遍——比你手动备份所有状态更可靠。

#### 2. Y-Flip 与 Scissor / Viewport

OpenGL 的 viewport / scissor 原点在 framebuffer **左下角**。ImGui 的 ClipRect 用屏幕坐标（左上角原点）。

```cpp
glViewport(x, fb_height - y - h, w, h);   // Y-flip
glScissor(x, fb_height - y - h, w, h);
```

DX11 / Vulkan / Metal 没有 Y-flip 问题。

#### 3. 深度缓冲

ImGui 不用深度测试。如果你在 ImGui 主 framebuffer 上做 3D，会**和后续 ImGui cmd 共享深度缓冲**。

3D 渲染**必须**：
- 进入时 `glClear(GL_DEPTH_BUFFER_BIT)` —— 清掉历史深度。
- 退出时 `glDisable(GL_DEPTH_TEST)` 让 ImGui 后续画时不被深度阻挡。

但是！3D 写入的深度会**残留**——下一个 ImGui Image 区域如果重叠，旧深度可能干扰。安全做法：每个 callback 进入时都 clear 自己 viewport 内的深度。

#### 4. AlphaBlending 状态

ImGui 用 `(SrcAlpha, OneMinusSrcAlpha)` 半透明混合。3D 渲染如果用了 `(One, One)`（加色混合）或关闭混合 → 必须在 callback 出口前还原。

### 性能比较

| 方案 | FBO 切换 | 状态切换 | 复杂度 |
|---|---|---|---|
| RTT + ImGui::Image | 每帧 1 次 | 0（ImGui 默认） | 简单 |
| Callback 嵌入 | 0 | 多次 | 复杂 |

**桌面 GPU**：RTT 方案 1 次 FBO 切换 ~ 0.5 ms 开销可忽略。callback 方案虽然不切 FBO，但 8~10 个状态切换也类似开销。
**移动 / VR GPU**：FBO 切换昂贵（~ 1-2 ms）。callback 方案明显省。

**实战建议**：99% 场景用 RTT + Image 简单可靠。只有遇到具体性能瓶颈才考虑 callback 嵌入。

---

## 5.3.7 ImTextureRef vs ImTextureID 选择

回到 1.92 的纹理 API 演进（第 0.4 章）：

```cpp
// 旧 API
ImGui::Image((ImTextureID)(uintptr_t)gl_handle, size);

// 1.92 新 API
ImGui::Image(ImTextureRef(gl_handle), size);
```

`ImTextureRef` 隐式构造接受 `ImTextureID`——所以**老代码仍能编译**，但官方推荐显式构造：

```cpp
ImTextureRef ref;
ref._TexID = (ImTextureID)(uintptr_t)gl_handle;
ImGui::Image(ref, size);
```

### `ImTextureRef` 的"延迟绑定"

```cpp
struct ImTextureRef {
    ImTextureData* _TexData;   // 如果 != NULL，使用这个
    ImTextureID    _TexID;     // 否则使用这个
    
    ImTextureID GetTexID() const {
        return _TexData ? _TexData->TexID : _TexID;
    }
};
```

**当 `_TexData != NULL`**：表示"这是一张 ImGui 管理的动态纹理"——backend 在 RenderDrawData 开头处理过状态，到 cmd 渲染时 `_TexData->TexID` 已经被 backend 填上。

**当 `_TexID != 0`**：表示"用户提供的现成纹理"——backend 直接用 `_TexID`。

**对你写自定义 widget 的影响**：通常用 `_TexID`。仅当你想引用 ImGui 内部的字体图集等时才用 `_TexData`。

---

## 5.3.8 实战：把 GBuffer 通道作为 ImGui 显示

调试 3D 渲染时常需要显示 GBuffer 的各个通道：

```cpp
ImGui::Begin("GBuffer Viewer");

ImGui::Text("Albedo:");
ImGui::Image(m_GBufferAlbedo, ImVec2(256, 144));

ImGui::Text("Normal:");
ImGui::Image(m_GBufferNormal, ImVec2(256, 144));   // RGB 是法线 XYZ ∈ [-1, 1] 但 image 期望 [0, 1]

ImGui::Text("Depth:");
ImGui::Image(m_DepthBuffer, ImVec2(256, 144));     // 深度是非线性

ImGui::End();
```

**问题**：

- Normal 是 [-1, 1] 范围的法线 → 默认 sampler 看起来像噪点（小于 0 的部分被裁剪）。
- Depth 是非线性的 [0, 1] → 远处物体显示几乎全白。

**解决**：用 callback 切到自定义 shader 做 remap：

```cpp
// NormalDebug.frag
#version 410 core
in vec2 vUV;
uniform sampler2D NormalTex;
out vec4 OutColor;

void main() {
    vec3 normal = texture(NormalTex, vUV).rgb;
    // [-1, 1] → [0, 1]
    vec3 vis = normal * 0.5 + 0.5;
    OutColor = vec4(vis, 1.0);
}

// DepthDebug.frag
#version 410 core
in vec2 vUV;
uniform sampler2D DepthTex;
uniform float NearPlane;
uniform float FarPlane;
out vec4 OutColor;

void main() {
    float d = texture(DepthTex, vUV).r;
    // 非线性深度还原为线性
    float linear_d = (2.0 * NearPlane) / (FarPlane + NearPlane - d * (FarPlane - NearPlane));
    OutColor = vec4(vec3(linear_d), 1.0);
}
```

```cpp
void DrawGBufferDebug(GLuint tex, const char* label, ShaderType remap) {
    ImGui::Text("%s:", label);
    
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ShaderRemapData* data = AllocateRemapData(remap, tex);
    
    dl->AddCallback(GBufferRemapCallback, data, sizeof(*data));
    ImGui::Image((ImTextureID)(uintptr_t)tex, ImVec2(256, 144), ImVec2(0,1), ImVec2(1,0));
    dl->AddCallback(ImDrawCallback_ResetRenderState, NULL);
}
```

——每个 GBuffer 通道用对应的 remap shader 渲染。Image 的几何（quad）由 ImGui 生成，shader 由我们替换。

---

## 5.3.9 调试 callback 出问题的 5 个步骤

callback 写错时症状千奇百怪——窗口闪烁、颜色错、几何丢失。排查清单：

### 1. 在 callback 内 print

```cpp
void MyCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd) {
    HZ_CORE_TRACE("MyCallback called! ClipRect=({},{}) to ({},{})",
        cmd->ClipRect.x, cmd->ClipRect.y, cmd->ClipRect.z, cmd->ClipRect.w);
    // ...
}
```

——确认 callback 被调用 + ClipRect 是预期的。

### 2. 用 RenderDoc 抓帧

每个 ImGui 渲染内部的 draw call 都能在 RenderDoc 看到。检查：
- callback 之前的 cmd 状态（program / vao / scissor）。
- callback 内你设的状态。
- callback 之后的 cmd 状态——是否完全恢复。

### 3. 暂时把 callback 改成 ResetRenderState

```cpp
// 原代码
dl->AddCallback(MyCallback, &data);
dl->AddRectFilled(...);
dl->AddCallback(ImDrawCallback_ResetRenderState, NULL);

// 测试代码
dl->AddCallback(ImDrawCallback_ResetRenderState, NULL);   // 占位
dl->AddRectFilled(...);
dl->AddCallback(ImDrawCallback_ResetRenderState, NULL);
```

——如果换掉后渲染正常，说明你的 MyCallback 内部破坏了状态。

### 4. ImGui Metrics 看 cmd 数量

```
Demo → Metrics → DrawLists → 选 list → CmdBuffer
```

每个 cmd 的 UserCallback 字段会显示在这里。如果你没看到预期的 callback cmd，说明 `AddCallback` 没生效——可能是写到错误的 DrawList 上。

### 5. 简化到最小 case

把整个 callback 简化到只 `glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);` —— 看到红色 = callback 在工作。然后逐步加你的真实代码，直到错误重现。

---

## 5.3.10 性能与陷阱清单

### 陷阱 1：忘记 ResetRenderState

```cpp
dl->AddCallback(MyCallback, ...);
// ❌ 没调 ResetRenderState
// 后续 ImGui cmd 用 MyCallback 设的 shader 画 → 全乱
```

### 陷阱 2：UserCallback 内调 ImGui API

```cpp
void MyCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd) {
    ImGui::Text("Hello");   // ❌ UB！callback 在 Render 阶段，已离开 frame scope
}
```

——callback 在 `RenderDrawData` 内部调用。此时 `g.WithinFrameScope == false`，调 `ImGui::XXX` 会崩。

### 陷阱 3：UserCallback 数据生命周期

```cpp
void DrawSomething() {
    GizmoData data = { ... };   // 栈上
    dl->AddCallback(MyCallback, &data);   // 存指针
    // 函数返回 → data 离开作用域
    // Render 时 callback 解引用 → UB
}
```

修复：用 `userdata_size > 0` 让 ImGui 复制数据：

```cpp
dl->AddCallback(MyCallback, &data, sizeof(data));
```

### 陷阱 4：嵌套 callback

ImGui 不支持 callback 嵌套——一个 cmd 只能有一个 UserCallback。如果你需要多层渲染状态，要么分多个 callback / 多个 cmd，要么在一个 callback 内处理全部。

### 陷阱 5：Multi-Viewport 下 callback 在错的 GL Context

Multi-Viewport 模式下，每个副 viewport 可能有自己的 GL Context。callback 触发时**当前 GL Context 是该 viewport 的**——你需要重新加载所有 GL 状态（shader handle / vao 在不同 context 不共享）。

简单做法：**禁止**在 multi-viewport 下用 callback，或者每个 GL Context 重新创建 shader 资源。

---

## 5.3.11 本章小结

`ImDrawCmd::UserCallback` 是 ImGui 与自家 GPU 渲染管线的"逃生口"——3 个层次的应用：

1. **作为纹理嵌入**：RTT + `ImGui::Image` —— 简单可靠，编辑器场景视图首选。
2. **替代 shader**：callback + ImGui 的几何 + 你的 shader —— 实现 dissolve / 扫光等特效。
3. **嵌入 3D 渲染**：callback 内直接调你的渲染管线 —— 最强但最复杂。

记住的 4 条规则：

1. **总是配对 `ResetRenderState`**：`AddCallback(my_cb)` 后必须 `AddCallback(ResetRenderState, NULL)`。
2. **`userdata_size > 0` 复制数据**：避免 callback 时数据已失效。
3. **callback 内不要调 ImGui API**：状态机已离开 frame scope。
4. **Y-Flip 注意**：OpenGL 的 viewport/scissor 原点在左下，ImGui 在左上。

---

## 5.3.12 下一章预告

第 5.4 章 [[22_第五部分_04_综合实战_HazelImGuiLayer]] 是本系列**最长**的一章——

- 把第 1 / 3 / 5 部分所有内容组合成**一份完整的 Hazel `ImGuiLayer` 代码**（>1000 行）。
- 包括 `OnAttach / OnDetach / Begin / End / OnEvent` 完整实现。
- 自研 Platform Backend（不依赖 imgui_impl_glfw）。
- 自研 Renderer Backend（不依赖 imgui_impl_opengl3）。
- Multi-Viewport 完整支持（PlatformIO + Renderer 全部回调）。
- 编辑器三件套（场景视图、属性面板、资源浏览器）共用工具函数。

读完 5.4 你应该能直接把代码 copy 进自己的 Hazel-fork，跑出一个**完整的编辑器**。这是本系列的实战终点。
