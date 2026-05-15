# MetaBindingGen — 两种前端的 C++/Lua 绑定代码生成器演示

这个工程演示了**如何用统一的中间表示**，同时支持两种不同的元数据来源方式：

1. **方案 A（独立 XML 风格）**：类似某些工业级引擎的风格，元数据写在单独的 XML 文件
2. **方案 B（源码内宏标注风格）**：类似 Unreal 风格，元数据直接写在 C++ 源码

两种方案**生成完全一样的 C++ 绑定代码**，下游完全透明。

---

## 目录结构

```
MetaBindingGen/
├── gen/                         # 代码生成器 (Python)
│   ├── meta_ir.py              # 中间表示 (ClassMeta / MethodMeta / MemberMeta)
│   ├── frontend_xml.py         # 方案 A 前端: XML 解析器
│   ├── frontend_cpp.py         # 方案 B 前端: C++ 宏扫描器
│   ├── backend_lua.py          # 后端: 生成 Lua 绑定 C++ 代码
│   └── main.py                 # 命令行入口
│
├── sample_xml/                  # 方案 A 的示例输入
│   ├── my_widget.h             # 纯净 C++ 头文件
│   └── my_widget.meta          # XML 元数据文件
│
├── sample_cpp/                  # 方案 B 的示例输入
│   └── my_widget.h             # 带 BIND_LUA_* 宏的 C++ 头文件
│
├── generated_xml/               # 方案 A 生成的代码
│   └── register_my_widget_to_lua.h
│
└── generated_cpp/               # 方案 B 生成的代码 (和上面内容一样!)
    └── register_my_widget_to_lua.h
```

---

## 使用方式

```bash
# 方案 A: 从 XML 生成
python gen/main.py --frontend xml --input sample_xml --output generated_xml

# 方案 B: 从 C++ 源码生成
python gen/main.py --frontend cpp --input sample_cpp --output generated_cpp

# 对比两个输出 (应该完全一致)
fc generated_xml\register_my_widget_to_lua.h generated_cpp\register_my_widget_to_lua.h
```

---

## 核心架构

```
    输入 (两种格式)          IR (统一中间表示)          输出 (统一目标代码)
    ─────────────           ──────────────────         ──────────────────
    my_widget.meta (XML)  ┐                                                
                          ├──> ClassMeta ──────────> register_my_widget_to_lua.h
    my_widget.h (宏标注)  ┘                                                
    
    frontend_xml.py        meta_ir.py              backend_lua.py
    frontend_cpp.py
```

**关键点：中间 IR 对两种输入完全相同**。所以后端代码生成器不需要知道数据是从哪来的。

---

## 扩展到更多场景

想加第三种前端？比如 JSON 格式的元数据？

1. 写一个 `frontend_json.py`，把 JSON 转成 `ClassMeta`
2. 在 `main.py` 里加一个 `--frontend json` 选项
3. **后端代码一行都不用改**

想加第三种后端？比如生成 Python 绑定（pybind11）？

1. 写一个 `backend_python.py`，消费 `ClassMeta` 生成 pybind11 代码
2. 在 `main.py` 里加 `--backend python`
3. **前端代码一行都不用改**

**这就是分层解耦的威力**。
