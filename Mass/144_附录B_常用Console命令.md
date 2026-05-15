# 附录 B — 常用 Console 命令

> RTS / Mass / 性能调试 高频命令清单。在 editor 或 game 控制台输入(`~` 键打开)。

---

## B.1 Mass 调试

```
mass.debug                       # Mass debug 总开关
mass.debug.entity                # 显示 entity ID
mass.debug.entity.fragment       # 显示 fragment 字段
mass.debug.archetype             # 列出所有 archetype
mass.debug.processor             # 列出 processor 与耗时
mass.debug.LOD                   # 显示 LOD level
mass.debug.visualize.archetype   # 颜色编码 archetype
mass.GameThreadTickInterval 0    # Mass 每帧 tick(默认)
```

## B.2 性能 / Stat

```
stat fps                  # FPS
stat unit                 # game / draw / GPU 时间
stat unitGraph            # 上面的图形版
stat unitMax              # 最大值
stat scenerendering       # 渲染统计
stat memory               # 内存总览
stat gc                   # GC 耗时
stat physics
stat audio
stat slowtask
stat namedevents          # 显示命名事件(需要打开 -trace=Cpu)

stat startfile            # 开始 stat 录制(.uestats)
stat stopfile             # 停止
```

## B.3 Profiler / Trace

```
trace.start Cpu Frame Mass Net Memory     # 开 trace
trace.stop                                  # 停
trace.bookmark "MyEvent"                    # 打标记

profilegpu                # 单帧 GPU profile
profilegpuHitches         # 自动捕获 hitch
```

## B.4 Render / Graphics

```
r.SetRes 1920x1080w         # 设分辨率(w 窗口 / f 全屏)
r.VSync 0                    # 关 VSync
r.FrameRateLimit 60          # 限帧
r.ScreenPercentage 100       # 渲染分辨率倍率
r.DynamicRes.OperationMode 1 # 动态分辨率

show staticmeshes            # 切换 SM 渲染
show particles               # 切换粒子
show postprocessing          # 切换后处理
show navigation              # 显示 navmesh
show collision               # 显示碰撞体

viewmode lit                 # 默认
viewmode unlit               # 无光照
viewmode wireframe           # 线框
viewmode shadercomplexity    # 着色器复杂度
```

## B.5 GC / 内存

```
gc.MultithreadedDestructionEnabled 1
gc.IncrementalGCEnabled 1
gc.IncrementalReachabilityTimeLimit 0.002
gc.CollectGarbageEveryFrame 0
obj gc                # 立即跑 GC
obj list class=AActor  # 列出所有 Actor
obj refs name=<NAME>   # 谁引用了这个对象
obj cycles             # 检测引用环
```

## B.6 Network

```
net.PackageMap.LongLoadThreshhold 0.02
net.MaxRPCPerNetUpdate 10
net.UseAdaptiveNetUpdateFrequency 1

net.Replication.DebugProperty <PropName>   # 调试某属性复制
net.Replication.EnableRecording 1           # 录制网络
showdebug net
```

## B.7 World / Streaming

```
streaming.numstreaminglevels   # 当前加载的 streaming level
wp.runtime.dumpstreaming       # World Partition streaming dump
wp.runtime.toggledrawcells     # 显示 cell

ce <event>                      # Console event(给 LevelBP 发事件)
open <map>                      # 切关卡
servertravel <map>              # server 切关卡
```

## B.8 Editor 专属

```
exit                # 退出
Test_DumpUStruct   # 转储某 USTRUCT 的反射信息
log <category> <verbosity>  # 改 log verbosity
```

## B.9 RTS / Mass 自定义建议

```
rts.SpawnUnits <count>     # 你自己实现的 spawn 命令
rts.SetFPSLimit <value>
rts.ToggleAI               # 关闭 AI 看渲染开销
rts.MassDumpStats          # 自定义 dump
```

---

**附录 B 完。下一附录:推荐阅读路线。**
