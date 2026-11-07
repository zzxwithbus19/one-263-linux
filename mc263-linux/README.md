# mc263-linux — 用 C++23 重写 build_262.py，产出 Minecraft 26.3 的 Linux 单文件

把仓库根目录的 Windows 构建脚本 `build_262.py`（Python + .NET/C# 自解压 exe）整体重写为
**C++23 工具链**，目标产物是 Linux 上的**一个 ELF 文件**：内置 Java 运行时、游戏库、
Linux natives、assets 与 mods，双击即玩；也可封装成 `.rpm`，安装后在应用菜单/命令行一键启动。

另外提供两块「开箱即用」能力：

- **winetricks 风格图形化配置页**（`make gui`）：浏览器里勾选组件、选版本/镜像/架构/内存/mods URL，
  一键完成「下载 → 构建 → 打 rpm」，实时进度与日志；无浏览器时 `--zenity` 用对话框完成同样流程。
- **网络下载器**（`mc263-fetch`）：从 Mojang 官方 CDN / BMCLAPI 镜像下载 version manifest、client jar、
  114 个依赖库与 natives、asset index 与 5147 个资源对象、官方 Java 25 运行时，全部 SHA1 校验 + 本地缓存 +
  并发下载；也可直接 `mc263-build --online` 一步到位。

> 目标平台：**Fedora 43+ / openSUSE Tumbleweed**（GCC ≥ 15、rpm ≥ 4.18、libzstd）。
> 本仓库**不含任何 Mojang 游戏文件**；构建者需用正版启动器下载的 26.3 文件自行生成成品。
>
> **路径安全（默认强制）**：所有写入目标默认只允许项目目录（沙箱根），
> `/`、`/home`、`/etc`、`/usr`、`/var`、`/opt` 等系统敏感路径一律拒绝，
> 且会先规范化路径以挡住 `..` 与符号链接逃逸。详见 [4.7 路径安全](#47-路径安全与输出目录)。
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

| `build_262.py`（Windows） | 本目录（Linux / 26.3） |
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
| curl CLI | `mc263-fetch` 的下载后端（fork+execv，不拼 shell） | 否 |
| zenity / xdg-open / 浏览器 | GUI 配置页（Web 模式或 `--zenity` 对话框模式） | 否 |
| python3 | 仅 `make check` 的 mock CDN / 端点诊断脚本 | 否 |

`USE_ZSTD=0` 时成品只依赖 glibc/libstdc++，可拿到其它发行版直接跑。

---

## 4. 快速开始

### 4.1 编译与自测（不需要游戏文件）

```bash
cd mc263-linux
make                 # C++23 编译 4 个工具：launcher / mc263-build / mc263-fetch / mc263-gui
make check           # 51 项端到端检查（全部离线可跑）
make info            # 查看编译器 / payload 格式 / 系统库
make gui             # 打开图形化配置页
```

`make check` 覆盖：版本 json 解析与 rules 过滤、签名剥离、DEFLATE jar 读取、`.so` 提取与内容、
JRE 裁剪与可执行位、assets/mods/config、v2 zstd 与 v1 STORED 双路径、缓存命中、
classpath 冒号分隔、JVM/游戏参数、日志、损坏 payload 被 CRC 干净拦截，
以及**离线 mock CDN** 下的下载器（SHA1、缓存、并发、进度、篡改自愈、`--online` 一步式）、
**GUI 任务编排**（配置读写、`/api/start`、状态流转、日志）与
**路径守卫**（`/`、`/home`、`/etc`、`/usr` 等 7 个敏感路径、符号链接逃逸、沙箱内放行、GUI/rpm 接口同样拒绝）。
调试时 `MC263_KEEP=1 make check` 会保留临时目录；`python3 tests/probe_endpoints.py` 可诊断真实下载端点。

### 4.2 图形化配置（winetricks 风格，推荐）

```bash
make gui                         # 自动打开浏览器配置页
make gui GUI_ARGS="--no-browser" # 只打印 URL，自己打开
build/mc263-gui --zenity         # 无浏览器：zenity 勾选清单 + 进度对话框
```

页面（`gui/index.html`）分区：

1. **组件勾选清单**：游戏主程序 / 依赖库+natives / 资源 / Java 运行时 / Fabric / 额外 mods / 打 rpm。
2. **基本设置**：版本（可从官方清单刷新）、下载源（official / bmclapi）、架构、并发数、玩家名、
   `-Xmx`、游戏文件目录、输出路径。
3. **额外 mods URL**：任意多个 `.jar` 直链，下载后随单文件打包。
4. **执行**：进度条 + 当前文件 + 实时日志，按钮「开始构建 / 仅保存配置 / 取消任务」。

配置持久化在 **`<项目目录>/.mc263/config.json`**（不再写 `~/.config`）。服务端只监听 `127.0.0.1`、
校验 Host、用 `fork+execv` 传参数（不拼 shell），任务顺序为 `mc263-fetch → mc263-build → build-rpm.sh`；
`POST /api/config` 会先校验 `mcDir` / `output` 是否在允许范围内。
`--zenity` 模式用 `--list --checklist` 选组件、`--entry` 填版本、`--progress` 显示进度，适合纯桌面会话。

### 4.3 网络下载（不需要预先准备游戏文件）

```bash
# 一键：下载 + 构建 + 打 rpm
make gui             # 页面上点「开始构建」

# 命令行两步（输出全部在项目目录内）
build/mc263-fetch --version 26.3                       # 默认下载到 ./gamefiles
build/mc263-build --mc-dir gamefiles --version 26.3 --output dist/mc263 --rpm

# 或者一步到位（构建前自动调用同目录的 mc263-fetch）
build/mc263-build --online --version 26.3 --mc-dir gamefiles --output dist/mc263
```

下载源（2026-11 实测，见 `tests/probe_endpoints.py`）：`piston-meta.mojang.com` 版本清单
（26.3 是当前 `latest.release`）、`piston-data.mojang.com` client jar、`libraries.minecraft.net`
（26.3 声明 114 个库）、`piston-meta` asset index（id=34，5147 个对象约 485 MB）、
`resources.download.minecraft.net` 资源对象、`launchermeta.mojang.com` Java 运行时清单
（`java-runtime-epsilon` / Java 25，147 个文件，含 `executable` 标志）、
`meta.fabricmc.net` Fabric loader（最新 0.20.1）。

特性：每个文件按官方 `sha1` 校验、已存在且校验通过即跳过（可断点续传/重复构建）、
`--jobs` 并发 1–16、`--mirror official|bmclapi|<http://自定义镜像>`、`--limit N` 小流量调试、
`--json-progress` 每行一个 JSON 供 GUI 解析、`--list-versions` 列出最近 release、
`--no-cache` 强制重下、`--allow-partial` 容忍个别失败。国内可加 `--mirror bmclapi`。

### 4.4 构建真正的单文件（已有游戏文件时）

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
| `--online` | 构建前自动调用同目录 `mc263-fetch` 下载缺失文件 |
| `--mirror` / `--jobs` | 配合 `--online`：下载源与并发数 |
| `--rpm` | 构建完直接调用 rpm 打包 |
| `--keep-signatures` / `--no-natives-scan` / `--verbose` | 调优与排查 |

构建结束会打印 `SHA256`，分发时请一并核对。

### 4.5 运行单文件

```bash
dist/mc263                               # 便携模式，存档在同目录
MC263_USERNAME=Alex dist/mc263           # 指定玩家名
dist/mc263 --username Steve --jvm-arg -Xmx6G
dist/mc263 --extract-only                # 只解压，不启动（排错用）
dist/mc263 --info                        # 查看 payload 格式 / 沙箱根 / 数据目录决策
cp dist/mc263 dist/Notch                 # 重命名即改玩家名
```

### 4.6 打成 rpm，安装后一键启动

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

### 4.7 路径安全与输出目录

`include/safe_path.hpp` 是统一守卫，四个工具在**任何目录创建 / 网络下载之前**调用：

| 规则 | 行为 |
|------|------|
| 沙箱根 | 默认 `mc263-linux/`（自动从可执行文件位置向上找 `Makefile` + `src/build_263.cpp`）；`MC263_SANDBOX_ROOT` 可覆盖 |
| 沙箱内 | 一律允许（即使项目本身位于 `/home/<user>/...` 下） |
| 沙箱外硬拒绝 | `/`、`/etc`、`/usr`、`/bin`、`/sbin`、`/lib*`、`/boot`、`/dev`、`/proc`、`/sys`、`/root`、`/var`、`/opt`、`/run`、`/srv`、`/mnt`、`/media`、`/home`（含子目录） |
| 沙箱外其它（如 `/tmp`、`/data`） | 默认也拒绝；`MC263_ALLOW_OUTSIDE=1` 才放行 |
| 规范化 | `weakly_canonical` 先解析 `..` 与符号链接，挡住逃逸 |
| 只读来源 | `--mc-dir`、`--java-runtime`、`--launcher` 不受限制（可指向官方 `~/.minecraft` 复用已下载文件） |

默认写入位置（**全部在项目目录内**）：

```
gamefiles/                 下载的游戏文件（mc263-fetch 默认 --mc-dir）
.cache/fetch/              下载元数据缓存
build/work/                构建暂存
dist/mc263                 输出单文件
dist/*.rpm                 rpm 产物
.mc263/config.json         GUI 配置（不再写 ~/.config）
runtime-data/{data,game}   运行时回退目录（XDG 的 /home 路径被拒时使用）
```

预检与调整：

```bash
build/mc263-fetch --check-paths                       # 只校验，不联网、不写文件
build/mc263-build --check-paths --output dist/mc263
MC263_SANDBOX_ROOT=/mnt/mydata make gui              # 把沙箱根换到专用数据盘
MC263_ALLOW_OUTSIDE=1 build/mc263-fetch --mc-dir /data/mc263   # 放行沙箱外非敏感路径
```

边界说明：rpm **安装阶段**的路径由包管理器管理（默认 `/opt/mc263`，`--system-links` 才写 `/usr`），
不属于构建工具的输出；用 `--prefix "$PWD/dist/rpm-root"` 可完全不写系统盘。
`--out` / `--topdir` 本身受守卫约束，`--out /etc` 会被直接拒绝。

---

## 5. 目录结构

```
mc263-linux/
├── Makefile                    C++23 / pkg-config / USE_ZSTD 开关 / check / gui / fetch / rpm
├── README.md
├── include/
│   ├── mini_zip.hpp            ZIP32 STORED 读写 + CRC32 + unix mode（构建器侧支持 zlib DEFLATE）
│   ├── mini_json.hpp           JSON 解析/生成（version json、config、进度事件）
│   ├── sfx_format.hpp          单文件尾部 32 字节 trailer（v1 STORED / v2 zstd）
│   ├── zstd_stream.hpp         系统 libzstd 流式压缩/解压
│   ├── download.hpp            curl 下载后端 + 自实现 SHA1 + 并发任务池 + 镜像替换
│   ├── safe_path.hpp           输出路径守卫（黑名单 + 沙箱根 + 符号链接逃逸防护）
│   └── fs_util.hpp             目录复制/裁剪、路径与日志工具
├── src/
│   ├── build_263.cpp           build_262.py 的 C++23 重写（支持 --online）
│   ├── launcher.cpp            C++23 自解压启动器（零/一 个第三方运行库）
│   ├── fetch_263.cpp           网络下载器 mc263-fetch
│   └── gui_263.cpp             本地 HTTP 配置服务 + zenity 回退 mc263-gui
├── gui/
│   └── index.html              winetricks 风格配置页（组件勾选 / 进度 / 日志）
├── packaging/
│   ├── mc263.spec              rpm spec（禁 strip、自动依赖）
│   ├── build-rpm.sh            打包 + 完整性校验 + 安装提示
│   ├── mc263.desktop           应用菜单项
│   └── mc263.svg               草方块图标
└── tests/
    ├── selftest.sh             42 项端到端自测（含 v1/v2、mock CDN、GUI 编排）
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
A：加 `--mirror bmclapi`（国内镜像）、`--jobs 12` 提高并发；重复执行只会补下校验失败的文件；
个别文件始终失败时可用 `--allow-partial` 先构建，或 `python3 tests/probe_endpoints.py` 诊断端点。
`--limit N` 可小流量验证，`--no-cache` 强制重新校验。

**Q：找不到 26.3 版本 / 想装别的版本？**
A：`build/mc263-fetch --list-versions` 列出最近 20 个 release；`--version latest` 跟最新正式版，
GUI 里点「刷新」从官方清单拉版本下拉。26.3 实测为当前 `latest.release`。

**Q：aarch64 没有官方 JRE？**
A：Mojang 的 JRE 清单目前只提供 `linux`（x86_64）；`mc263-fetch` 在 aarch64 会跳过 runtime 下载，
构建时用系统 Java 或 `--java-runtime` 指定自备 JRE（一样会复制进单文件）。

**Q：GUI 页面没自动打开？**
A：`make gui GUI_ARGS="--no-browser"` 打印 URL 手动打开；纯命令行/无桌面用
`build/mc263-gui --zenity`（需 zenity），或直接用 `mc263-fetch` + `mc263-build` 命令行。
服务只监听 `127.0.0.1`，关闭终端或 Ctrl+C 即停止。

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

## 8. 法律声明

本项目**不包含** Minecraft 游戏文件、Java 运行时或任何 Mojang/Microsoft 版权素材，
仅提供构建工具。`mc263-fetch` 只从 Mojang 官方 CDN / 公共镜像下载公开资源文件，
**不提供账号认证、不绕过任何验证**；运行游戏仍需构建者自己拥有正版 Minecraft 账号。
成品由构建者使用自己通过正版渠道获取的文件生成，分发责任由构建者承担。
`packaging/mc263.spec` 中的 `License` 字段指本工具链代码，请按你仓库的实际许可证调整。
