# mc263 — Minecraft 26.3 Linux 启动器（Qt6 / C++23）

> 许可：GPL-3.0-or-later（全文见 [LICENSE](LICENSE)）

HMCL 形态的启动器：**一个 ELF + 旁边的 `minecraft/` 与 `.mc263/`**。
Qt6 Widgets 原生界面（无 Electron、无内嵌浏览器、无 moc），启动时自动选择
Wayland / XWayland，KDE Plasma 6 这类只给 Wayland 的桌面也能原生运行。

```
mc263-qt                 ← 唯一可执行文件（约 530 KB）
minecraft/               ← 游戏目录（首次运行自动建，可在设置里改）
.mc263/
├── config.json          版本 / 内存 / Java / 镜像 / 组件（改完即存）
└── mc263_downloader.py  内嵌 python3 下载脚本（运行时释放）
```

> 装在 `/usr/bin` 这类只读位置时，这两个目录自动改放 `~/.local/share/mc263/`。

---

## 1. 安装

主线是 **Arch Linux**。AUR 上没有（也不打算上），装法全部走命令行：

| 方式 | 适合 | 怎么做 |
|---|---|---|
| 本机出包再装 | 最快，版本自己控 | `bash packaging/build-arch.sh` → `sudo pacman -U dist/arch/mc263-*.pkg.tar.zst` |
| 自建 pacman 源 | 多机、要能 `pacman -Sy` 升级 | 见 [第 6 节](#6-打包与分发) |
| `install.sh` | Release 里已有 `.pkg.tar.zst` | `sh install.sh`（Arch 自动走 pacman） |
| 独立 ELF | 免安装 / 非 Arch | 下 `mc263-qt-26.3-x86_64`，`chmod +x` 直接跑 |

### 1.1 Arch Linux（推荐）

依赖自己装——构建脚本只构建，**不联网、不替你 `pacman -S`**：

```bash
sudo pacman -S --needed base-devel qt6-base qt6-wayland python curl

bash packaging/build-arch.sh                      # 等价：make arch
sudo pacman -U dist/arch/mc263-26.3-1-x86_64.pkg.tar.zst
```

装完有：`/usr/bin/mc263-qt`、应用菜单项、图标、`LICENSE`、AppStream 元数据。
卸载 `sudo pacman -R mc263`；用户数据在 `~/.local/share/mc263/`，卸载不会删。

配过自建源（[第 6 节](#6-打包与分发)）的话更省事：

```bash
sudo pacman -Sy && sudo pacman -S mc263
```

### 1.2 一条命令（从 Release 装）

```sh
curl -fsSL https://raw.githubusercontent.com/zzxwithbus19/one-263-linux/main/install.sh | sh
```

- **Arch（有 `pacman`）**：下 `.pkg.tar.zst` → `sudo pacman -U`，Qt6 / python / curl 从官方源补
- **Fedora / openSUSE（`dnf` / `zypper`）**：下 rpm → `sudo dnf install`（COPR 版排队慢，见 [第 6 节](#6-打包与分发)）
- **都没有**：退回**独立 ELF**，装到 `~/.local/bin/mc263-qt`（root 则 `/usr/local/bin`）
- 完整性：优先用 GitHub 给每个 asset 附的 `sha256:` digest，拿不到再退回 `SHA256SUMS`
- 资产名不固定也没关系：脚本走 GitHub API 按规则挑，release 里叫什么都能装上

强制走某条路 / 指定 tag（含 pre-release）/ 卸载：

```sh
sh install.sh --pacman       # 只装 Arch 的 .pkg.tar.zst（sudo pacman -U）
sh install.sh --rpm          # 只装 rpm
sh install.sh --elf          # 只装独立 ELF，不碰包管理器
sh install.sh --tag pre非生产  # 装指定 tag（pre-release 也支持）
sh install.sh --uninstall    # 卸载（pacman 包 / rpm / ELF 都会处理）
```

> `releases/latest` 只指向**非 pre-release**；仓库里只有 pre-release 时脚本会自动退回
> `/releases` 列表取最新一个，也可以用 `--tag` 明确指定。
> 想马上装但 release 里还没 `.pkg.tar.zst`：走 1.1，本机出包当场就能装。

### 1.3 免安装 ELF

`mc263-qt-26.3-x86_64` 是单独一个二进制，没有菜单项和图标，运行时自己备：

```bash
sudo pacman -S --needed qt6-base qt6-wayland python curl   # Arch
chmod +x mc263-qt-26.3-x86_64
./mc263-qt-26.3-x86_64
```

> 自备了 Qt6 运行时的其它发行版也能这么跑；但它们目前**不提供打包支持**，
> Fedora / openSUSE 请用 rpm。

---

## 2. 从源码构建

C++23 + Qt6 Widgets 手写界面，**不用 CMake、不用 moc**，`make` 一条命令的活。

### 2.1 依赖

| 用途 | Arch 包 | 说明 |
|---|---|---|
| 编译 | `base-devel`、`qt6-base` | g++ / make 与 `pkg-config(Qt6Widgets)` |
| 运行 | `qt6-wayland` | Wayland 原生；不装会回退 XWayland |
| 运行 | `python`、`curl` | 内嵌下载器与下载后端（都 exec 系统命令） |
| 可选 | `java-runtime` | 不装也能用启动器自己下的 Mojang 运行时 |

```bash
sudo pacman -S --needed base-devel qt6-base qt6-wayland python curl
```

Fedora / openSUSE 对应：`qt6-qtbase-devel`、`qt6-qtwayland` / `libQt6WaylandClient6`、`python3`、`curl`。

### 2.2 编译、测试、运行

```bash
make                 # → build/mc263-qt
make check           # 57 项核心单测 + 7 项内嵌下载器自测（全部离线，不联网）
make info            # 编译器 / Qt6 / 产物路径

build/mc263-qt                                        # 运行
QT_QPA_PLATFORM=offscreen timeout 3 build/mc263-qt    # 无图形环境冒烟；退出码 124 = 正常跑着
```

编译只链接 `Qt6Widgets / Qt6Gui / Qt6Core`：单个 ELF，没有 moc 产物、没有外部资源文件。

### 2.3 出包

```bash
make arch            # pacman 包：源码包 → sha256 → makepkg（见第 6 节）
make rpm             # rpm：调 packaging/build-rpm.sh（rpm 主线在 fedora 分支）
```

改代码前先翻 [docs/arch-handoff.md](docs/arch-handoff.md)：分层、约定、验收清单都在里面。

---

## 3. 界面与功能

| 页面 | 能力 |
|------|------|
| 启动游戏 | 本地版本下拉、玩家名、一键启动；`QProcess` 异步 exec java，日志实时刷新 |
| 下载游戏 | 在线版本列表、组件勾选、official / bmclapi 镜像、并发线程（默认 10）；JSON 进度条与日志；**清除下载缓存**按钮（只删元数据缓存，已下载的 jar / 库 / 资源不受影响） |
| Mod 下载 | 搜 Modrinth：关键词 + 加载器 + 游戏版本过滤，**排序可选下载量 / 相关度 / 关注数 / 最新发布 / 最近更新**；双击即装，SHA1 校验、已存在则跳过 |
| 加载器 | Fabric / Quilt 版本列表拉取 + 一键安装：抓 profile json、与原版 json 合并、下载依赖到 `libraries/`，生成 `versions/<版本>+<加载器>/` |
| Java 与兼容性 | 后台线程扫描配置 / Mojang JRE / `JAVA_HOME` / `PATH` / `/usr/lib/jvm`；11 项查验（架构、Java 版本、依赖库、natives、图形音频库、内存、磁盘、路径守卫，可选网络连通性） |
| 游戏目录 | 独立一页：自动扫描 ELF 旁 / XDG 数据目录 / `~/.minecraft`，列出各自的版本数与 mods 数，选中即切换，避免两套装各用一份 |
| 设置 / 内存 | **内存默认交给 JVM 管理**（不传 `-Xmx`/`-Xms`）；关掉自动后可手动拖 `-Xmx`/`-Xms`。四段式内存条：程序占用 → 缓存 → 游戏堆 → 余下可用，2 秒刷新、超可用内存红字提示；额外 JVM 参数；**游戏语言默认跟随系统 locale**（识别不到就 `en_us`）；页底是许可与说明（README 链接、仓库链接、显示后端） |

**版本隔离（始终开启，没有关闭开关）**：启动时 `--gameDir` 指向 `versions/<版本>/`，
`mods`、`saves`、`resourcepacks`、`shaderpacks`、`logs` 都在该目录下自动建好 ——
不同版本（含 `26.3` 与 `26.3+fabric`）互不影响，存档和 Mod 不会串。
Mod 下载也装进对应版本的目录，而不是全局 `minecraft/mods/`。
升级前已有的存档不会自动搬（避免动用户文件）：需要时把 `minecraft/saves`、`mods` 移到 `versions/<版本>/` 下同名目录。

**游戏目录（独立页面）**：自动扫描 ELF 旁的 `minecraft/`、安装版的
`~/.local/share/mc263/minecraft`、`~/.minecraft` 与当前配置，列出各自的版本数 / mods 数，
选中即切换（也可「浏览…」手选）；启动时若发现多个目录都存有版本，会在状态栏提示。

**游戏语言**：启动参数带 `--language`。默认**跟随系统 locale**（依次看 `LC_ALL` → `LC_MESSAGES` →
`LANG` → `LANGUAGE`，去掉编码与 modifier 后映射到 Minecraft 的语言代码，例如 `zh_CN.UTF-8` → `zh_cn`、
`zh_TW` → `zh_tw`）；识别不到或系统是 `C`/`POSIX` 就用 `en_us`。也可以在「设置 / 内存」里手动指定，
选择会存进 `config.json`。

下载逻辑：内嵌 python3 脚本（`ThreadPoolExecutor` 多线程 + SHA1 校验 + 本地缓存），
Qt 通过 `QProcess` 读 JSON 进度；Java 扫描与兼容性查验在 `std::thread` 后台跑，
用 `QMetaObject::invokeMethod` 回主线程刷新 UI。

---

## 4. 启动前的 Java 检查

启动时先扫 Java，用整页遮住主界面（不是弹窗，点不穿），达标才放行。

| 版本 | 需要的 Java |
|------|-------------|
| 26.x（主线） | **25** |
| 1.20 ~ 1.21 | 21 |
| 1.17 ~ 1.19 | 17 |
| 1.12 及更早 | 8 |

阈值优先读已下载的 `<版本>.json` 里的 `javaVersion.majorVersion`，没下载时按版本号推断
（`core::requiredJavaMajor`，有单测）。

- 找到达标 Java → 直接进主界面
- 只有 Java 8 / 11 → 「当前 Java 11 已不再被 Minecraft 26.3 所支持」
- 只有 17 / 21 → 「当前 Java 21 低于 Minecraft 26.3 要求的 Java 25」
- 一个都没有 → 提示去下载页，那里「Java 运行时」**蓝色高亮并默认勾选**

卡片按钮：下载 Java 运行时 / 重新检测 / 手动指定 java… / 退出。
下载完成或手动指定后自动重扫，达标即放行；点「启动游戏」时按当前版本再算一次。

---

## 5. Wayland 原生

KDE Plasma 6 起默认只有 Wayland 会话，XWayland 可能根本没装。启动器在
`QApplication` 之前就选好 Qt 平台插件（`core::choosePlatform`，有离线单测）：

| 情形 | 结果 |
|------|------|
| 已设 `QT_QPA_PLATFORM`（或命令行 `-platform`） | 完全尊重，不干预 |
| Wayland 会话 + 装了 wayland 插件 | `wayland`，原生协议，不经过 XWayland |
| Wayland 会话 + 只有 xcb 插件 | 回退 `xcb`（需 XWayland），stderr 提示补装 |
| 两个插件都缺 | 打印 Fedora / openSUSE 的安装命令后退出，不让 Qt qFatal |

配套：`setDesktopFileName("mc263-qt")` 让 Wayland 的 `app_id` 与桌面项一致（任务栏分组、图标）；
HiDPI、分数缩放、多屏交给 Qt6；打开链接走 `xdg-open` / portal。
「关于」页会显示当前后端与 Qt 版本，可直接确认。

---

## 6. 打包与分发

> **主线是 Arch**：`packaging/PKGBUILD` + `packaging/build-arch.sh`，从源码构建，
> **不走 AUR**——不需要账号、不需要审核，全部命令行完成。交接说明见
> [docs/arch-handoff.md](docs/arch-handoff.md)。
> rpm 线（`build-rpm.sh` / 完整 spec / COPR）在 `fedora` 分支维护，发版走 COPR：
> 会自动给各 Fedora 版本构建，但**排队慢**——等不及就本地 `bash packaging/build-rpm.sh`，
> 或在 CI 里直接出 rpm。`main` 上只留 `packaging/mc263.spec` 作参考。

### 6.1 Arch Linux：pacman 包（主线）

构建依赖自己装（脚本只构建，**不联网、不替你 `pacman -S`**）：

```bash
sudo pacman -S --needed base-devel qt6-base qt6-wayland python curl
bash packaging/build-arch.sh          # 等价：make arch
#   → dist/arch/mc263-26.3-1-x86_64.pkg.tar.zst
#   流程：make-source.sh 出 dist/mc263-26.3.tar.gz → 算 sha256 填进 PKGBUILD 副本
#         → makepkg（跑 make check）→ 校验包内容 → offscreen 冒烟（退出码 124 为正常）
```

装到本机：

```bash
sudo pacman -U dist/arch/mc263-26.3-1-x86_64.pkg.tar.zst
```

常用开关（都是 `build-arch.sh` 的参数）：

| 开关 | 作用 |
|------|------|
| `MC263_REL=2` | 内容改了但版本号不变：`pkgrel` +1，否则 pacman 认为还是同一个包 |
| `--repo` | 顺带用 `repo-add` 建本地 pacman 仓库（见 6.2） |
| `--nodeps` | 跳过依赖检查（缺包但想先出包时用；依旧不联网） |
| `--no-check` | 跳过 `make check`（不推荐） |
| `--clean` | 先清空 `dist/arch/` |

包内容：`/usr/bin/mc263-qt`、desktop 文件、hicolor 图标、`LICENSE`、AppStream 元数据；
`depends=('qt6-base' 'qt6-wayland' 'python' 'curl')`，`optdepends` 里是 `java-runtime`。
依赖由 pacman 从官方源解析，包本身不静态链接 Qt。

### 6.2 自建 pacman 仓库（没有 AUR 账号也能装）

```bash
bash packaging/build-arch.sh --repo   # 或：bash packaging/make-repo-arch.sh
#   → dist/arch/repo/：mc263.db.tar.zst、mc263.files.tar.zst、*.pkg.tar.zst、mc263.conf
```

把 `dist/arch/repo/` 整个目录丢到任意静态托管（GitHub Pages / nginx / 对象存储），
用户把 `mc263.conf` 里那三行追加到 `/etc/pacman.conf`（`Server` 换成实际 URL，
`$arch` 保留），然后：

```bash
sudo pacman -Sy && sudo pacman -S mc263
```

仓库不签名，所以用 `SigLevel = Optional TrustAll`；要签名就 `repo-add --sign`，
把公钥一起发，用户 `pacman-key --add` 后再调高 `SigLevel`。

### 6.3 Release 该传哪些

| 资产 | 用途 |
|------|------|
| `mc263-26.3-1-x86_64.pkg.tar.zst` | Arch 主发布物，`install.sh` 自动挑这个 |
| `mc263-qt-26.3-x86_64` | 独立 ELF，免安装；无菜单项、需自备 Qt6 与 python3 |
| `SHA256SUMS`（可选） | 离线核对；GitHub 自带 digest 时可不传 |
| `mc263-26.3-1.fc45.x86_64.rpm`（可选） | Fedora / openSUSE，见 `fedora` 分支 |

### 6.4 rpm（Fedora / openSUSE，COPR 发布 + 本地快出）

rpm 里放的是**预编译二进制**：打包装包机要 Qt6 开发包（编译用），
**安装的人不需要**，`dnf` / `zypper` 会自动拉 Qt6 运行时。

```bash
sudo dnf install rpm-build gcc-c++ make qt6-qtbase-devel
bash packaging/build-rpm.sh
#   → dist/mc263-26.3-1.fc45.x86_64.rpm   正式包（带发行版后缀）
#   → dist/mc263-26.3-1.x86_64.rpm        稳定名副本（install.sh 用）
#   → dist/mc263-qt-26.3-x86_64           独立 ELF
#   → dist/SHA256SUMS                     所有产物的摘要（离线兜底）
```

不需要 root 跑 rpmbuild（`_topdir` 固定到项目内 `build/rpmbuild`）。

两个常用开关：

- `--release 2`：版本号不变但内容改了，必须 +1，否则 `dnf` 认为是同一个包、不会升级
- `--sign <指纹>`：加 GPG 签名（RPM 6 内置，不需要 `rpm-sign`）；不加就是未签名包，
  Fedora 45 装的时候要走 `--nogpgcheck`（`install.sh` 会自动处理）

包内容：`/usr/bin/mc263-qt`、desktop 文件、hicolor 图标、`LICENSE`。
Qt 的库依赖由 rpm 从 ELF 的 `DT_NEEDED` 自动生成；另加 `Requires: python3`，
`Recommends: qt6-qtwayland`（openSUSE 分支写 `libQt6WaylandClient6`）。

**rpm 线的 Release 资产**

| 资产 | 用途 |
|------|------|
| `mc263-26.3-1.fc45.x86_64.rpm` | 主发布物，装 `/usr/bin` + 菜单项，依赖自动拉 |
| `mc263-26.3-1.x86_64.rpm` | 稳定名副本，`install.sh` 的固定 URL |
| `mc263-qt-26.3-x86_64` | 独立 ELF，免安装；无菜单项、需自备 Qt6 与 python3 |
| `SHA256SUMS`（可选） | 离线核对；GitHub 自带 digest 时可不传 |
| `mc263-signing-key.asc`（可选） | 公钥，签名验证用 |

---

## 7. Fedora 45 / RPM 6.1 的签名要求

Fedora 45 起 RPM 默认 `%_pkgverify_level all`：未签名包不再只是 `NOKEY` 警告，
而是 `package … does not verify` **直接拒绝**。

`install.sh` 已做四级降级：

```
dnf install → --nogpgcheck → --no-gpgchecks（DNF5 本地包回归 issue #2614）
           → rpm -Uvh --replacepkgs --nosignature
```

手动等价：

```bash
sudo dnf install --nogpgcheck ./mc263-26.3-1.x86_64.rpm
sudo dnf install --no-gpgchecks ./mc263-26.3-1.x86_64.rpm
sudo rpm --define '_pkgverify_level digest' -Uvh --nosignature ./mc263-26.3-1.x86_64.rpm
# 永久放宽本机：
echo '%_pkgverify_level digest' | sudo tee /etc/rpm/macros.verify
```

想自己签名（RPM 6 内置，不需要 `rpm-sign`）：

```bash
gpg --quick-generate-key "mc263 <you@example.com>" rsa4096 sign never
bash packaging/build-rpm.sh --sign <密钥指纹或邮箱>
#   → dist/mc263-signing-key.asc（自动导出）
# 用户侧：sudo rpmkeys --import mc263-signing-key.asc
```

`rpm-setup-autosign` 只适合本机构建机自动签名，解决不了别的机器的信任问题。

---

## 8. 目录结构

```
mc26-one/
├── Makefile                    all / check / rpm / arch / info / clean
├── README.md
├── install.sh                  一键安装（pacman / rpm / ELF 自动选择 + 摘要校验）
├── include/
│   ├── launcher_core.hpp       布局 / 配置 / Java 探测 / 兼容性 / 启动 / meminfo / 版本隔离 / 加载器 / 目录扫描
│   ├── download.hpp            curl 下载后端 + SHA1 + 并发池（兼容性网络检查用）
│   ├── modrinth.hpp            Modrinth 搜索 / 版本 / 下载（公开 API，无需 key）
│   ├── mini_json.hpp           JSON 解析与生成
│   ├── mini_zip.hpp            ZIP 读取（扫 natives）
│   ├── safe_path.hpp           路径守卫（黑名单 + 沙箱根 + 符号链接逃逸防护）
│   └── fs_util.hpp             目录 / 文件 / 环境变量小工具
├── src/
│   └── mc263_qt.cpp            Qt6 主窗口（6 页，内嵌 python3 下载脚本）
├── docs/
│   └── arch-handoff.md         Arch 打包与分发交接（无 AUR 的命令行路线）
├── packaging/
│   ├── PKGBUILD                Arch 包模板（主线，源码构建）
│   ├── build-arch.sh           源码包 → PKGBUILD → makepkg → 校验 → 冒烟
│   ├── make-source.sh          生成 dist/mc263-<版本>.tar.gz（PKGBUILD 的 source）
│   ├── make-repo-arch.sh       用 repo-add 生成自建 pacman 仓库
│   ├── mc263.metainfo.xml      AppStream 元数据（GNOME Software / KDE Discover）
│   ├── mc263.spec              rpm spec（参考；完整线与 COPR 在 fedora 分支）
│   ├── build-rpm.sh            make → rpmbuild → dist/*.rpm + 独立 ELF + SHA256SUMS
│   ├── mc263.desktop           应用菜单项（Exec=mc263-qt）
│   └── mc263.svg               图标
└── tests/
    ├── core_test.cpp           57 项核心单测
    ├── downloader_test.sh      内嵌下载器 + mock CDN 自测（7 项）
    ├── mock_cdn.py             离线模拟 Mojang 下载端点（真实 SHA1）
    └── probe_endpoints.py      真实端点诊断
```

---

## 9. 常见问题

**Q：提示需要 Java 25，但我装了 17？**
A：26.3 要 25（1.20+ 要 21、1.17+ 要 17）。点「下载 Java 运行时」拿 Mojang 官方 25，
或「手动指定 java…」指向更高版本。

**Q：没有 Java 能先下游戏吗？**
A：能。检测不到 Java 时，下载页会自动出现蓝色「Java 运行时」并默认勾选，下完即可启动。

**Q：`qt.qpa.plugin: Could not load the Qt platform plugin "wayland"`？**
A：缺 `qt6-qtwayland`（openSUSE 是 `libQt6WaylandClient6`）。装上即可；
临时也可 `QT_QPA_PLATFORM=xcb build/mc263-qt` 走 XWayland。

**Q：Wayland 下没图标 / 任务栏分组不对？**
A：从 rpm 安装就有桌面项；源码直接跑时没有 `mc263.desktop`，不影响功能。

**Q：内存条里「缓存」和「游戏 -Xmx」什么关系？**
A：缓存是可回收的文件页缓存；灰块长度就是 `-Xmx`，从缓存之后起算。
右边轨道是启动后仍可用。设太大先挤缓存，再不够会红字提示 OOM 风险。

**Q：下载慢 / 个别文件失败？**
A：下载页把「下载源」切到 `bmclapi`，并发调到 12，再点一次——已存在且 SHA1 一致的文件自动跳过。
`python3 tests/probe_endpoints.py` 可诊断真实端点。

**Q：能登录正版账号吗？**
A：**暂时不能**。启动参数是离线模式（`--accessToken 0 --userType legacy`），只能玩单人 / 局域网。
正版登录需要 Microsoft OAuth → Xbox Live → XSTS → Minecraft Services，尚未实现。

---

## 10. 许可与法律声明

本项目源码采用 **GPL-3.0-or-later**。上游 `zhanbalao/mc26-one` 未声明许可证，
且其唯一的 `build_262.py` 已从本仓库移除；当前代码为全新 C++23 实现，不含上游代码。

Qt6 为 `LGPL-3.0-only OR GPL-3.0-only WITH Qt-GPL-exception-1.0`，本项目按
**LGPL-3.0-only** 动态链接使用：LGPL 与 GPLv3 兼容，项目许可不变。
分发二进制时需随附 LGPL-3.0 文本、声明使用了 Qt、保持动态链接以便替换 Qt 库，
并提供 Qt 源码获取途径。`mc263.spec` 的 `License` 为 `GPL-3.0-or-later`（不含 Qt 代码本体）。

本仓库**不包含** Minecraft 游戏文件、Java 运行时或任何 Mojang / Microsoft 版权素材。
下载器只从 Mojang 官方 CDN 或公共镜像获取公开资源文件，
**不提供账号认证、不绕过任何验证**；运行游戏需要你自己拥有正版 Minecraft。
下载到本地的游戏文件、Mojang Java 运行时与第三方库各自适用其原有许可。
