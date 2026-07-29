# 单文件 Minecraft 26.2 启动器（纯净原版）

将 Minecraft 26.2（1.21.x）打包成一个**零依赖的 .exe 文件**——内置 Java 25 运行时、所有库和资源文件，扔到任何 Windows x64 电脑上直接运行。

## 成品效果

- **262.exe** (~765 MB) — Minecraft 26.2 纯净原版
- 玩家名 = exe 文件名（重命名即改名）
- 存档保存在 exe 同级 `saves/` 目录

## 原理

```
┌─────────────────────────────────────────────┐
│                262.exe                      │
│  ┌───────────────────────────────────────┐  │
│  │  .NET 10 Self-Contained Launcher      │  │
│  ├───────────────────────────────────────┤  │
│  │  内嵌 ZIP (~695 MB)                   │  │
│  │  ├─ app/      所有 .jar 库 + mc.jar   │  │
│  │  ├─ runtime/  Java 25 JRE (100 MB)    │  │
│  │  └─ assets/   游戏资源文件 (455 MB)   │  │
│  └───────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
```

首次运行：自解压到 `_data/` → 启动内嵌 Java → 启动 Minecraft。

## 前置条件

无。Windows x64 电脑即可。

## 构建（可选）

如需自行构建，需要：

| 工具 | 用途 |
|------|------|
| .NET 10 SDK | 编译 C# 启动器 |
| Python 3.8+ | 运行构建脚本 |
| 正版 Minecraft | 获取游戏文件 |

### 1. 获取游戏文件

用官方启动器下载 Minecraft 26.2，确保完整运行。之后在 `.minecraft/` 下会有：
- `versions/26.2/26.2.jar`
- `libraries/...`
- `assets/...`
- `runtime/java-runtime-epsilon/`

### 2. 配置构建脚本

编辑 `build_vanilla_262.py` 开头的路径：

```python
MC_DIR = Path(r"D:\.minecraft")           # 你的 .minecraft 目录
JAVA_RT = Path(r"D:\java-runtime-epsilon") # Java 25 运行时
BASE = Path(r"D:\mc_package_build")        # 构建工作目录
```

### 3. 运行构建

```cmd
python build_vanilla_262.py
```

输出：`F:\beif\Desktop\MC\262\262.exe`

## 自定义

### 修改 JVM 参数

编辑 `build_vanilla_262.py` 中 C# 模板的 `jvmArgs` 字符串：

```
-Xms2G -Xmx4G -XX:+UseZGC ...
```

### 修改启动参数

`--gameDir` 控制存档位置，默认 exe 同级目录。

## 常见问题

**Q: 杀毒软件报毒？**
A: 自解压单文件 exe 常被误报。这是合法的 .NET 程序。

**Q: 游戏崩溃？**
A: 检查 `launcher.log`（exe 同目录）获取 Java 输出。

## 法律声明

本项目**不包含** Minecraft 游戏文件。构建者需自行通过正版 Minecraft 启动器获取游戏文件。
