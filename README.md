# mc26-one — Minecraft 26.3 的 Linux 单文件构建器（C++23）

> **许可：GPL-3.0-or-later**（全文见 [`LICENSE`](LICENSE)）

本仓库是原 Windows 构建脚本 `build_262.py`（Python + .NET/C# 自解压 exe，**已从仓库移除**）的
**C++23 重写**，源码位于仓库根目录。目标产物是 Linux 上的**一个 ELF 文件**：内置 Java 运行时、
游戏库、Linux natives、assets 与 mods，双击即玩；也可封装成 `.rpm`，安装后在应用菜单/命令行一键启动。

另外提供 **Qt6 原生启动器**（`make qt`）：一个 ELF + 应用目录下的 `minecraft/` 与 `.mc263/`，
自带 Python3 多线程下载（Mojang / BMCLAPI 镜像，SHA1 校验 + 缓存）、Java 探测与闸门、
兼容性查验、内存图形化配置，形态参考 HMCL。详见 [4.6](#46-qt6-原生启动器hmcl-形态)。

> 目标平台：**Fedora 43+ / openSUSE Tumbleweed**（GCC ≥ 15、rpm ≥ 4.18、libzstd）。
> 本仓库**不含任何 Mojang 游戏文件**；构建者需用正版启动器下载的 26.3 文件自行生成成品。
>
> **路径安全（默认强制）**：所有写入目标默认只允许项目目录（沙箱根），
> `/`、`/home`、`/etc`、`/usr`、`/var`、`/opt` 等系统敏感路径一律拒绝，
> 且会先规范化路径以挡住 `..` 与符号链接逃逸。详见 [4.5 路径安全](#45-路径安全与输出目录)。
> 工具不会再往 `~/.config`、`~/.cache`、`~/.local/share`、`~/mc263` 写任何东西。

---

## 1. 成品是什么

```
mc263                       ← 一个文件（约 600–800 MB，取决于 assets）
┌───────────────────────────────────────────────┐
│ C++23 自解压启动器 ELF（约 300 KB）            │
├───────────────────────────────────────────────┤
│ game.zip（STORED，逐文件保留 unix 权限）       │
│   app/      mc.jar + 全部依赖 jar + natives/*.so│
│   runtime/  Linux JRE（bin/java 带可执行位）   │
│   assets/   资源与索引                         │
│   mods/     Fabric mods（首次运行装到游戏目录） │
│   config.json  版本/主类/assetIndex/JVM 参数   │
├───────────────────────────────────────────────┤
│ 32 字节 trailer: magic + size + CRC32 + 版本   │
└───────────────────────────────────────────────┘
```

默认 payload 再用系统 **libzstd** 压缩（格式 v2），减小体积；`USE_ZSTD=0` 可回退到
v1 纯 STORED，此时启动器**零第三方运行库依赖**（只依赖 glibc/libstdc++）。

首次运行：校验 CRC → 解压到数据目录 → 修可执行位 → 安装 mods → 拼 classpath →
`fork+exec` 内嵌 Java。二次启动靠 `.mc263-ready` 标记直接跳过解压，秒开。

两种运行形态：

| 形态 | 数据目录 | 存档/mods | 场景 |
|------|----------|-----------|------|
| 便携（exe 目录可写） | `<exe目录>/_data` | `<exe目录>/saves`, `mods` | 双击单文件，沿用 Windows 版习惯 |
| 系统（rpm 装到 /opt，只读） | `<沙箱>/runtime-data/data`；不可写时 `/tmp/mc263-<uid>/data` | 同左的 `game/` | 执行 `<prefix>/mc263` |

所有运行时写入同样经过路径守卫：原先的 XDG 路径（`~/.cache`、`~/.local/share`）因落在 `/home`
默认被拒绝，会自动回退到沙箱内的 `runtime-data/`；rpm 场景沙箱根只读时回退 `/tmp/mc263-<uid>`
（会打印提示，重启后可能丢失，需持久化请设置 `MC263_SANDBOX_ROOT` 指向专用目录）。

玩家名优先级：`--username` > `MC263_USERNAME` > config 默认值 > **可执行文件名**（重命名即改名，
与 Windows 版一致）> `$USER`。日志在 `<gameDir>/launcher.log`。

---

## 2. 与原 Python 脚本的对照

| 原 `build_262.py`（Windows，已移除） | 本仓库（Linux / 26.3） |
|---------------------------|------------------------|
| 硬编码 `F:\beif\...` 路径 | `--mc-dir` / `--java-runtime` / `--output` 等 CLI + 环境变量 |
| `shutil.copytree(JAVA_RT)` | `copyRuntime()`，自动探测 Mojang JRE / JAVA_HOME / 系统 java，裁剪 man/src.zip/classes.jsa |
| 剥离版本 jar 的 `META-INF/*.SF|DSA|RSA|EC` | `repackVersionJar()`，用 zlib DEFLATE 重写 jar |
| **硬编码 26.2 库表（约 80 行字符串）** | **解析 `versions/<ver>/<ver>.json` 的 libraries/rules/assetIndex/mainClass**，兼容 Fabric maven 坐标 |
| `*-natives-windows.jar` → `.dll` | `*-natives-linux*.jar` → `.so`（按 x86_64 / aarch64 选 classifier） |
| `;` 分隔 classpath、java.exe | `:` 分隔、`bin/java`、`-Djava.library.path` / LWJGL / JNA / Netty 目录参数 |
| C# 模板字符串 + `.csproj` + `dotnet publish` | 预编译 C++ 启动器 ELF + `config.json`（版本参数不写死在代码里） |
| .NET 内嵌资源 `game.zip` | payload **追加到 ELF 尾部**（ELF 加载器忽略尾部字节）+ trailer |
| 输出 `262+fo.exe` | 输出 `mc263`，可选 `--rpm` 生成安装包 |

---

## 3. 环境要求

```bash
# Fedora
sudo dnf install gcc-c++ make zlib-devel libzstd-devel rpm-build desktop-file-utils python3
# openSUSE Tumbleweed
sudo zypper install gcc-c++ make zlib-devel libzstd-devel rpm-build desktop-file-utils python3
```

| 依赖 | 用途 | 是否进成品运行时 |
|------|------|------------------|
| zlib (`-lz`) | 构建器读取 Maven jar、重写版本 jar | 否 |
| libzstd (`-lzstd`) | 压缩/解压 payload（v2） | 是（rpm 自动声明 `libzstd.so.1()(64bit)`） |
| rpmbuild / zstd / sha256sum | 打 rpm、系统压缩、摘要 | 否 |
| python3 | Qt 启动器的下载器（运行时释放到 `.mc263/`）+ `make check` 的 mock CDN | 否 |

`USE_ZSTD=0` 时成品只依赖 glibc/libstdc++，可拿到其它发行版直接跑。

---

## 4. 快速开始

### 4.1 编译与自测（不需要游戏文件）

```bash
# 在仓库根目录执行
make                 # C++23 编译 2 个工具：launcher / mc263-build
make qt              # 另加 Qt6 原生启动器 build/mc263-qt
make check           # 39 项端到端检查 + 46 项核心单测（全部离线可跑）
make info            # 查看编译器 / payload 格式 / 系统库
```

`make check` 覆盖：版本 json 解析与 rules 过滤、签名剥离、DEFLATE jar 读取、`.so` 提取与内容、
JRE 裁剪与可执行位、assets/mods/config、v2 zstd 与 v1 STORED 双路径、缓存命中、
classpath 冒号分隔、JVM/游戏参数、日志、损坏 payload 被 CRC 干净拦截、
**路径守卫**（`/`、`/home`、`/etc`、`/usr` 等 7 个敏感路径、符号链接逃逸、沙箱内放行、rpm 接口同样拒绝），
以及从 Qt 源码里抽取内嵌 python3 下载器、在**离线 mock CDN** 上验证下载（SHA1、多线程、进度）。
调试时 `MC263_KEEP=1 make check` 会保留临时目录；`python3 tests/probe_endpoints.py` 可诊断真实下载端点。

### 4.2 构建真正的单文件（已有游戏文件时）

先用官方启动器下载并完整运行一次 Minecraft **26.3**（Linux 版），确保有：

```
~/.minecraft/versions/26.3/26.3.json + 26.3.jar
~/.minecraft/libraries/...            （含 -natives-linux jar）
~/.minecraft/assets/...
~/.minecraft/runtime/java-runtime-epsilon/.../bin/java   （Linux JRE，不是 java.exe）
```

然后（`--mc-dir` 是**只读来源**，可以指向官方启动器目录；输出必须留在项目内）：

```bash
make
build/mc263-build \
  --mc-dir ~/.minecraft \
  --version 26.3 \
  --output dist/mc263
```

Fabric 整合版（例如版本目录叫 `263+fo`）同理，用 `--version 263+fo`；
若第三方启动器没把库放进 `libraries/`，加 `--all-libs` 全量打包或多次 `--extra-lib PATH`。

常用参数：

| 参数 | 说明 |
|------|------|
| `--mc-dir` / `--version` / `--version-dir` | 游戏数据目录、版本目录名、直接指定版本目录 |
| `--java-runtime PATH` | 显式指定含 `bin/java` 的 Linux JRE |
| `--arch x86_64\|aarch64` | 目标架构（默认本机 `uname -m`），决定 natives classifier |
| `--all-libs` | 打包 `libraries/` 全部 jar（不依赖 version json 的保底模式） |
| `--mods-dir` / `--extra-lib` / `--extra-jvm-arg` | 可重复追加 mods 目录 / 单个 jar / JVM 参数 |
| `--launcher` / `--output` / `--work-dir` | 启动器 ELF、输出文件、暂存目录 |
| `--rpm` | 构建完直接调用 rpm 打包 |
| `--keep-signatures` / `--no-natives-scan` / `--verbose` | 调优与排查 |

构建结束会打印 `SHA256`，分发时请一并核对。

### 4.3 运行单文件

```bash
dist/mc263                               # 便携模式，存档在同目录
MC263_USERNAME=Alex dist/mc263           # 指定玩家名
dist/mc263 --username Steve --jvm-arg -Xmx6G
dist/mc263 --extract-only                # 只解压，不启动（排错用）
dist/mc263 --info                        # 查看 payload 格式 / 沙箱根 / 数据目录决策
cp dist/mc263 dist/Notch                 # 重命名即改玩家名
```

### 4.4 打成 rpm，安装后一键启动

```bash
bash packaging/build-rpm.sh --payload dist/mc263       # rpm 输出到 dist/，或 make rpm
sudo dnf install dist/mc263-26.3-1.fc45.x86_64.rpm    # Fedora
sudo zypper install dist/mc263-26.3-1.*.rpm           # openSUSE TW
```

默认只把文件装到 `%{mc263_prefix}`（默认 `/opt/mc263`），**不写 `/usr`**：执行
`/opt/mc263/mc263` 即启动。需要应用菜单/命令链接时显式开启（这是用户主动的系统安装行为）：

```bash
bash packaging/build-rpm.sh --payload dist/mc263 --system-links        # 额外写 /usr/bin 与桌面项
bash packaging/build-rpm.sh --payload dist/mc263 --prefix "$PWD/dist/rpm-root"  # 完全不碰系统目录
```

打包脚本会用 `rpm -qp --dump` 的 SHA256 与源单文件比对，确保 rpm 的 strip 流程没有削掉
ELF 尾部的 payload（spec 里用 `%global __os_install_post %{nil}` 禁用该流程）。
rpm 净负荷也用系统 zstd（`w19.zstdio`）压缩，老版本 rpm 自动回退 xz。
`.rpm` 产物本身只写到 `dist/`（或 `--out` 指定的沙箱内目录），`--out /etc` 之类会被拒绝。

### 4.5 路径安全与输出目录

`include/safe_path.hpp` 是统一守卫，构建器与 Qt 启动器在**任何目录创建 / 网络下载之前**调用：

| 规则 | 行为 |
|------|------|
| 沙箱根 | 默认**仓库根目录**（自动从可执行文件位置向上找 `Makefile` + `src/build_263.cpp`）；`MC263_SANDBOX_ROOT` 可覆盖 |
| 沙箱内 | 一律允许（即使项目本身位于 `/home/<user>/...` 下） |
| 沙箱外硬拒绝 | `/`、`/etc`、`/usr`、`/bin`、`/sbin`、`/lib*`、`/boot`、`/dev`、`/proc`、`/sys`、`/root`、`/var`、`/opt`、`/run`、`/srv`、`/mnt`、`/media`、`/home`（含子目录） |
| 沙箱外其它（如 `/tmp`、`/data`） | 默认也拒绝；`MC263_ALLOW_OUTSIDE=1` 才放行 |
| 规范化 | `weakly_canonical` 先解析 `..` 与符号链接，挡住逃逸 |
| 只读来源 | `--mc-dir`、`--java-runtime`、`--launcher` 不受限制（可指向官方 `~/.minecraft` 复用已下载文件） |

默认写入位置（**全部在项目目录内**）：

```
gamefiles/                 游戏文件默认来源（--mc-dir 的示例位置）
build/work/                构建暂存
dist/mc263                 输出单文件
dist/*.rpm                 rpm 产物
.mc263/config.json         Qt 启动器配置（不再写 ~/.config）
runtime-data/{data,game}   运行时回退目录（XDG 的 /home 路径被拒时使用）
```

预检与调整：

```bash
build/mc263-build --check-paths --output dist/mc263   # 只校验，不写文件
MC263_SANDBOX_ROOT=/mnt/mydata build/mc263-qt        # 把沙箱根换到专用数据盘
MC263_ALLOW_OUTSIDE=1 build/mc263-build --mc-dir /data/mc263 --output dist/mc263  # 放行沙箱外非敏感路径
```

边界说明：rpm **安装阶段**的路径由包管理器管理（默认 `/opt/mc263`，`--system-links` 才写 `/usr`），
不属于构建工具的输出；用 `--prefix "$PWD/dist/rpm-root"` 可完全不写系统盘。
`--out` / `--topdir` 本身受守卫约束，`--out /etc` 会被直接拒绝。

### 4.6 Qt6 原生启动器（HMCL 形态）

和前面「把游戏塞进 ELF」不同，这是标准启动器形态 —— **一个 ELF + 应用目录下的游戏目录 + 配置目录**：

```
mc263-qt                      ← 单个 ELF（Qt6 Widgets，无 moc / Q_OBJECT）
minecraft/                    ← 应用目录下的游戏目录，首次运行自动建立
.mc263/
├── config.json               版本 / 内存 / Java / 镜像 / 组件
└── mc263_downloader.py       内嵌 python3 下载脚本（运行时释放）
```

```bash
make qt                                   # 构建 build/mc263-qt（需 qt6-qtbase-devel）
make USE_QT6=1                            # 默认构建也包含 Qt 启动器
build/mc263-qt                            # 桌面运行
QT_QPA_PLATFORM=offscreen build/mc263-qt  # 无图形环境冒烟测试
```

| 页面 | 能力 |
|------|------|
| 启动游戏 | 本地版本下拉、玩家名、一键原版启动；QProcess 异步 exec java，日志实时显示。启动前先过 Java 闸门（见下） |
| 下载游戏 | 在线版本列表、组件勾选、official/bmclapi 镜像、并发线程（默认 10）；JSON 进度条与日志。「Java 运行时」不是常驻选项，只在 Java 不达标时以蓝色高亮出现并默认勾选 |
| Java 与兼容性 | 后台线程扫描 JAVA_HOME / PATH / Mojang JRE / `/usr/lib/jvm`，列出版本与来源；查验架构、Java 版本、依赖库、natives、图形/音频库、内存、磁盘、路径守卫，可选网络连通性 |
| 设置 / 内存 | 改完即存（400 ms 防抖，状态栏回显时间），没有「保存配置」按钮；架构按本机自动判断，不再让用户选。四段式内存条：程序占用（蓝）→ 缓存（浅蓝）→ 游戏 `-Xmx`（浅灰，长度就是分配量）→ 余下可用；2 秒自动刷新，琥珀三角标出分配右边界，显示占比与启动后预计剩余，超出可用内存时红字提示 OOM 风险；另有 `-Xms`、JVM 参数与游戏目录 |
| 关于 | 本地 `README.md` 蓝字链接（用系统默认程序打开）、仓库链接、当前显示后端与 Qt 版本 |

**多线程模型**（主线程不做任何耗时操作）：下载交给内嵌的 python3 脚本，
脚本用 `ThreadPoolExecutor` 并发下载 + SHA1 校验 + 本地缓存，Qt 通过 `QProcess` 异步读取 JSON 进度；
Java 扫描与兼容性查验在 `std::thread` 后台执行，用 `QMetaObject::invokeMethod` 回主线程刷 UI；
启动游戏用 `QProcess` 异步执行。核心逻辑全部在 `include/launcher_core.hpp`（不依赖 Qt），
由 `tests/core_test.cpp` 做 46 项离线单测。

#### 启动前的 Java 闸门

启动器不再让用户自己勾「要不要下 Java」，而是在 `QApplication` 起来后先扫一遍 Java，
用**整页遮住主界面**（不是弹窗，点不穿）直到条件满足：

| 版本 | 要求的 Java |
|------|-------------|
| 26.x（本仓库主线） | **25** |
| 1.20 ~ 1.21 | 21 |
| 1.17 ~ 1.19 | 17 |
| 1.12 及更早 | 8 |

阈值不是写死的：优先读已下载的 `<版本>.json` 里的 `javaVersion.majorVersion`，
没下载时按版本号推断（`core::requiredJavaMajor`，有单测）。

- 找到达标的 Java → 直接放行进主界面
- 只找到更老的版本 → 卡片标题写「需要 Java 25 及以上（当前 Java 11）」，
  正文按情况说明：8/11 提示「已不再被 Minecraft 所支持」，17/21 提示「低于该版本要求」
- 一个都没有 → 提示去下载页，那里「Java 运行时」会**蓝色高亮并默认勾选**
- 卡片上有「下载 Java 运行时 / 重新检测 / 手动指定 java… / 退出」；
  下载完成或手动指定后会自动重新扫描，达标即自动放行
- 点「启动游戏」时会用当前选中版本再算一次阈值，不达标就把闸门拉回来

#### Wayland 原生（KDE Plasma 6 / GNOME 46+ 等只给 Wayland 的桌面）

KDE Plasma 6 起默认只提供 Wayland 会话，XWayland 可能根本没装，所以 `mc263-qt`
在 `QApplication` 之前就按会话选好 Qt 平台插件（`core::choosePlatform`，有离线单测）：

| 情形 | 结果 |
|------|------|
| 已设 `QT_QPA_PLATFORM`（或命令行 `-platform`） | 完全尊重，不干预 |
| Wayland 会话 + 装了 wayland 插件 | `wayland`，原生协议，不经过 XWayland |
| Wayland 会话 + 只有 xcb 插件 | 回退 `xcb`（需 XWayland），启动时在 stderr 提示补装 |
| 两个插件都缺 | 打印 Fedora / openSUSE 的安装命令后退出，而不是让 Qt qFatal |

配套细节：`setDesktopFileName("mc263-qt")` 让 Wayland 的 `app_id` 与桌面项一致
（窗口分组、任务栏图标、通知归属）；HiDPI、分数缩放、多屏由 Qt6 自己处理；
打开仓库链接与「打开游戏目录」都走 `QDesktopServices` / `xdg-open`，在 Wayland 下经 portal 生效。

```bash
sudo dnf install qt6-qtwayland        # Fedora：不装就只有 xcb 回退
sudo zypper install libQt6WaylandClient6   # openSUSE TW
QT_QPA_PLATFORM=xcb build/mc263-qt    # 想强制走 XWayland 时
build/mc263-qt --platform wayland      # 强制原生（Qt 自身参数，优先级最高）
```

「关于」页底部会显示 `显示后端：wayland（原生 Wayland，不经过 XWayland）· Qt 6.x.y`，
可以直接确认当前跑在哪个后端上。

> 若 stderr 出现 `Failed to register with host portal ... App info not found for 'mc263-qt'`：
> 只是系统里没有同名的 `mc263-qt.desktop`（源码直接运行时正常），不影响窗口、下载和启动。
> 想让任务栏图标与窗口分组完全正确，装一个同名桌面项即可。

---

## 5. 目录结构

```
mc26-one/（仓库根目录）
├── .gitignore                 忽略回收站/记忆目录/构建产物/运行期数据
├── Makefile                    C++23 / USE_ZSTD / USE_QT6 / check / qt / rpm
├── README.md
├── include/
│   ├── mini_zip.hpp            ZIP32 STORED 读写 + CRC32 + unix mode（构建器侧支持 zlib DEFLATE）
│   ├── mini_json.hpp           JSON 解析/生成（version json、config、进度事件）
│   ├── sfx_format.hpp          单文件尾部 32 字节 trailer（v1 STORED / v2 zstd）
│   ├── zstd_stream.hpp         系统 libzstd 流式压缩/解压
│   ├── download.hpp            curl 下载后端 + 自实现 SHA1 + 并发任务池 + 镜像替换
│   ├── safe_path.hpp           输出路径守卫（黑名单 + 沙箱根 + 符号链接逃逸防护）
│   ├── launcher_core.hpp       HMCL 式核心：布局/配置/Java 探测/兼容性/启动/内嵌下载（不依赖 Qt）
│   └── fs_util.hpp             目录复制/裁剪、路径与日志工具
├── src/
│   ├── build_263.cpp           原 Windows 脚本的 C++23 重写（离线打包）
│   ├── launcher.cpp            C++23 自解压启动器（零/一 个第三方运行库）
│   └── mc263_qt.cpp            Qt6 原生启动器（内嵌 python3 下载脚本）
├── packaging/
│   ├── mc263.spec              rpm spec（禁 strip、自动依赖）
│   ├── build-rpm.sh            打包 + 完整性校验 + 安装提示
│   ├── mc263.desktop           应用菜单项
│   └── mc263.svg               草方块图标
└── tests/
    ├── selftest.sh             39 项端到端自测（v1/v2、mock CDN、路径守卫）
    ├── core_test.cpp           Qt 版核心 46 项离线单测（Wayland / xcb 后端选择、meminfo 口径、Java 版本阈值）
    ├── mock_cdn.py             离线模拟 Mojang 全部下载端点（真实 SHA1）
    └── probe_endpoints.py      真实端点诊断（版本清单 / JRE / Fabric / 镜像）
```

---

## 6. C++23 与系统库的使用说明

按 Fedora / openSUSE TW 的新工具链设计，代码使用了 C++23 的
`std::print/println`（`<print>`）、`std::string::contains/ends_with`、`std::ranges::sort`，
编译标准为 `-std=c++23`。系统侧用 `pkg-config` 探测 zlib / libzstd，用 `rpmbuild`、
`sha256sum`、`curl`、`zenity`、`xdg-open`、`desktop-file-utils` 等发行版自带工具；**没有引入任何第三方头文件依赖**
（环境里也没有 libarchive/jsoncpp/nlohmann/libzip，因此 ZIP 与 JSON 用两个轻量 header-only
实现，启动器侧保持可独立运行）。

`libstdc++-static` 在 Fedora 默认不安装，故默认动态链接 libstdc++；
Makefile 提供 `STATIC_CXX=1`（装了静态库时可用）。C++23 二进制在 Fedora 43+ / TW 上可直接运行，
rpm 会自动生成 `GLIBCXX_*` / `libzstd.so.1()(64bit)` 依赖。

---

## 7. 常见问题

**Q：启动报找不到 `libGL.so.1` / X11 / OpenAL 之类？**
A：natives 里的 LWJGL `.so` 仍需要发行版的图形/音频库。Fedora：
`sudo dnf install mesa-libGL mesa-libEGL libX11 libXext libXcursor libXrandr libXi libXxf86vm alsa-lib`；
openSUSE TW 对应 `Mesa-libGL1` 等包。rpm 无法自动声明这些 `dlopen` 依赖。

**Q：`~/.cache` 挂载了 `noexec`，内嵌 java 无法执行？**
A：用 `MC263_DATA_DIR=/path/on/exec/mount` 换一个可执行分区，或 `MC263_PORTABLE=1` 走便携模式。

**Q：下载慢、个别文件失败、或官方 CDN 连不上？**
A：Qt 启动器的下载页里把「下载源」切到 `bmclapi`（国内镜像）、把「并发线程」调到 12；
再点一次「开始下载」只会补下校验失败的文件。`python3 tests/probe_endpoints.py` 可诊断真实端点。

**Q：找不到 26.3 版本 / 想装别的版本？**
A：Qt 启动器「下载游戏」页点「获取在线版本」从官方清单拉下拉（最近 20 个 release），
也可以直接手输版本号。26.3 实测为当前 `latest.release`。

**Q：aarch64 没有官方 JRE？**
A：Mojang 的 JRE 清单目前只提供 `linux`（x86_64）。Qt 启动器在 aarch64 会跳过运行时下载，
用系统 Java 即可；单文件构建时用 `--java-runtime` 指定自备 JRE（一样会复制进单文件）。

**Q：没有桌面，怎么下游戏文件？**
A：用官方启动器先下好一份 26.3，再用 `mc263-build --mc-dir ~/.minecraft` 打包；
或者在有桌面的机器上跑 `build/mc263-qt` 下载到 `minecraft/` 再拷过来。

**Q：为什么 `--output ~/mc263` / `--mc-dir /etc/...` 被拒绝？想把数据放到别的盘怎么办？**
A：默认禁止把输出写到 `/`、`/home`（含主目录下的散落路径）、`/etc`、`/usr` 等敏感位置。
把输出留在项目内（`dist/`、`gamefiles/`）即可；需要换位置就设
`MC263_SANDBOX_ROOT=/mnt/mydata`（整棵输出树迁过去），或 `MC263_ALLOW_OUTSIDE=1` 放行沙箱外的非敏感路径。
`/`、`/etc`、`/usr`、`/home` 是硬黑名单，**没有开关可以绕过**；`--check-paths` 可先预检。
运行时的存档/解压目录同理，被拒时会自动回退到 `<沙箱>/runtime-data` 或 `/tmp/mc263-<uid>` 并打印提示。

**Q：某些 Java 参数报不认识（例如 `-XX:+UseCompactObjectHeaders`）？**
A：这是 Java 25 的选项。用 `--java PATH` 指定其它 JRE，或构建时 `--extra-jvm-arg` 覆盖；
`config.json` 也可在解压后手动改（注意 `--force-extract` 会覆盖）。

**Q：库缺失 / 启动即崩？**
A：看 `<gameDir>/launcher.log` 里的完整命令行；构建日志里的「缺失声明库」列表；
优先试 `--all-libs`。也可 `--extract-only` 后检查 `_data/app/` 下的 jar 与 `.so`。

**Q：想让成品完全不带第三方运行库依赖？**
A：`make clean && make USE_ZSTD=0`，用该 launcher 重新构建即可产出 v1 单文件。

---

## 8. 许可与法律声明

### 8.1 许可

本仓库的工具链源码采用 **GNU General Public License v3.0 或更高版本**
（SPDX `GPL-3.0-or-later`），全文见根目录 [`LICENSE`](LICENSE)。
`packaging/mc263.spec` 的 `License` 字段与打出的 rpm（内含 LICENSE 文件）保持一致。

上游 `zhanbalao/mc26-one` 未声明任何许可证；其唯一的脚本 `build_262.py` 已从本仓库移除，
README 亦为重写，因此本仓库现有代码是全新的 C++23 实现，不含上游代码。
本许可仅覆盖本仓库自身代码。

**Qt6 启动器（`src/mc263_qt.cpp`）** 额外动态链接 Qt6 Widgets/Gui/Core。按 Fedora 包元数据，
Qt6 为 `LGPL-3.0-only OR GPL-3.0-only WITH Qt-GPL-exception-1.0`，此处按 **LGPL-3.0-only** 使用：
LGPL 与 GPLv3 兼容，**项目许可不变**。分发 Qt 版时需履行 LGPL 附带义务 —— 随附 LGPL-3.0
许可文本、明确声明使用 Qt（「关于」页已写明）、保持动态链接以便用户替换 Qt 库、
按 LGPL 提供 Qt 源码获取途径（发行版源码包或 Qt 官方）。若修改了 Qt 库本身需回馈。
`mc263-qt` 是构建期/本机工具，不进入 rpm 包，因此 `mc263.spec` 的 `License` 字段保持
`GPL-3.0-or-later`；若将来把它也打进 rpm，需改为 SPDX 组合表达式并附带对应许可文本。

### 8.2 第三方素材

本项目**不包含** Minecraft 游戏文件、Java 运行时或任何 Mojang/Microsoft 版权素材，
仅提供构建工具。Qt 启动器的下载器只从 Mojang 官方 CDN / 公共镜像获取公开资源文件，
**不提供账号认证、不绕过任何验证**；运行游戏仍需构建者自己拥有正版 Minecraft 账号。
成品由构建者使用自己通过正版渠道获取的文件生成，分发责任由构建者承担。
打包进单文件的第三方素材（Minecraft jar / assets、Mojang Java 运行时、第三方依赖库与 mods）
作为独立数据聚合分发，各自适用其原有许可，不因本工具而改为 GPLv3。
