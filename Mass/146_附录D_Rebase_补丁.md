# 附录 D — 上游 Rebase 与项目 Patch 管理

> UE 5.7.3 → 5.8 → 5.9 ... 升级时,如何让自己改动不冲突。

---

## D.1 现状

```
你的 fork = Epic UnrealEngine 5.7.3 + 自己的修改
   ↓ 几个月后
Epic 出 5.8 → 几千个 commit → 部分文件改了
   ↓
git rebase upstream/release → 冲突
```

---

## D.2 减少冲突的工程纪律

```
1. 改 Engine 代码越少越好
   - 优先用 Plugin / Subsystem 扩展
   - 必须改时,加最少行数
   - 改之前留 // PROJECT-XXX: 标记
   
2. 不格式化引擎代码
   - 你 IDE 自动格式化 → 几千行噪声 diff
   - 关掉自动格式化,或仅格式化你写的新文件
   
3. 项目代码和引擎代码分开
   - 你的项目放 Project/Plugins/<YourPlugin>/
   - 不放 Engine/ 或 Engine/Plugins/
   
4. 配置文件放 Project/Config/
   - 不改 Engine/Config/(冲突高频区)
```

---

## D.3 Rebase 流程

```bash
# 1. 备份当前
git branch backup-pre-58 release

# 2. 添加 upstream
git remote add upstream https://github.com/EpicGames/UnrealEngine
git fetch upstream

# 3. Rebase
git checkout release
git rebase upstream/release

# 冲突 → 一个个解
# 每个文件 → 看 PROJECT-XXX 标记,保留你的改动

# 4. 测试
Setup.bat
GenerateProjectFiles.bat
Build.bat UnrealEditor Win64 Development
```

---

## D.4 用 Patch 系列管理

```bash
# 把改动整理成 Patch(单独 commit 一个文件)
git format-patch upstream/release -o patches/

# patches/ 目录下:
0001-PROJECT-Mass-RTS-customization.patch
0002-PROJECT-Tick-skip-empty-archetypes.patch
0003-PROJECT-Iris-quantization-3byte.patch

# Rebase 时如果某 patch 被上游覆盖 → 跳过
# 否则 apply

git am patches/*
```

---

## D.5 Hooks / CI

```
预提交 hook 检查:
   - 不改 Epic 文件除非有 PROJECT-XXX 注释
   - 编码 UTF-8
   - 不用 std::vector / new(用 TArray / NewObject)

CI 自动:
   - 每周从 upstream pull,跑测试
   - 冲突早发现
```

---

**附录 D 完。下一附录:术语表。**
