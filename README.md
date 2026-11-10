# mc263 — Minecraft 26.3 Linux 启动器（Qt6 / C++23）

> 许可：GPL-3.0-or-later（ [LICENSE](LICENSE)）
>
> 分支：**`fedora`（本分支）= RHEL / Fedora 的 rpm 线**（rpmbuild + COPR）；
> `main` = Arch 主线（PKGBUILD + pacman 自建源）。源码两边同步，只有打包产物不同。

包形态**一个 ELF + 旁边的 `minecraft/` 与 `.mc263/`**。
Qt6 Widgets 原生界面（无 Electron、无内嵌浏览器、无 moc），启动时自动选择
Wayland 原生运行。

```
mc263-qt                 ← 唯一可执行文件（约 600 KB）
minecraft/               ← 游戏目录（首次运行自动建，可在设置里改）
.mc263/
├── config.json          版本 / 内存 / Java / 镜像 / 组件（改完即存）
└── mc263_downloader.py  内嵌 python3 下载脚本（运行时释放）
```

> 装在 `/usr/bin` 这类只读位置时，这两个目录自动改放 `~/.local/share/mc263/`。

---

## 1. 安装

本分支面向 **Fedora / openSUSE**。AUR 上没有（没账户），Arch 用户走 `main`；COPR 倒是有。

### 1.1 一条命令

```sh
curl -fsSL https://raw.githubusercontent.com/zzxwithbus19/one-263-linux/fedora/install.sh | sh
```

强制走某条路 / 指定 tag（含 pre-release）/ 卸载：

```sh
sh install.sh --rpm          # 只装 rpm
sh install.sh --elf          # 只装独立 ELF，不碰包管理器
sh install.sh --tag pre非生产  # 装指定 tag（pre-release 也支持）
sh install.sh --uninstall    # 卸载（rpm 与 ELF 都会处理）
```

脚本先读 `/etc/os-release` 判断发行版再决定路线（不看 `uname`——Fedora 上可以装
CachyOS 内核，`uname -r` 里带 cachyos，但系统还是 Fedora）：

| 系统 | 默认动作 |
|---|---|
| Fedora 系 | **不下载，直接打印 COPR 的 `dnf copr enable` 两条命令**（未签名 rpm 在 45+ 会被拒装） |
| Arch 系（含 CachyOS / Manjaro） | `pacman -U` 装 `.pkg.tar.zst` |
| openSUSE 系 | **不下载，直接打印 COPR 的 `zypper addrepo` 三条命令**（同一个项目有 Tumbleweed chroot） |
| 其它 | 退回独立 ELF |

想强制走某条路就加参数（例如 Fedora 上 `--rpm` 会真的下 rpm，`--elf` 装免安装版）。

**签名失败就失败**：脚本不会自动 `--nogpgcheck`、不会退化到 `rpm --nosignature`、
也不会改 `/etc/rpm`，只把可选命令打出来让你自己决定（见第 6.4 节）。

### 1.2 COPR 源（推荐，Fedora 官方密钥签名）

```sh
sudo dnf copr enable cxc123/one-263-linux
sudo dnf install mc263
```

`dnf` 会自动导入并验证签名，不需要 `--nogpgcheck`。

**openSUSE Tumbleweed** 用同一个项目的 `opensuse-tumbleweed` chroot：

```sh
sudo rpm --import https://download.copr.fedorainfracloud.org/results/cxc123/one-263-linux/pubkey.gpg
sudo zypper addrepo https://download.copr.fedorainfracloud.org/results/cxc123/one-263-linux/opensuse-tumbleweed-x86_64/ one-263-linux
sudo zypper refresh one-263-linux
sudo zypper install mc263
```

COPR 的 openSUSE 仓库有个细节：**rpm 是签名的，但仓库元数据（`repomd.xml`）没有 `.asc`**，
所以第一步先 `rpm --import` 把公钥装进来，免得 zypper 卡在未知密钥上。
这不是跳过校验：包签名照样验。之后升级走 `zypper dup` 或 `zypper update mc263` 都行。

### 1.3 手动下载 / 免安装 ELF

| 发行版 / 方式 | 文件 | 做法 |
|---|---|---|
| Fedora / openSUSE | `mc263-26.3-1.<arch>.rpm` | `sudo dnf install ./mc263-26.3-1.x86_64.rpm` |
| 免安装 | `mc263-qt-26.3-<arch>` | `chmod +x` 后直接跑 |

独立 ELF 没有菜单项和图标，需要自己准备 Qt6 运行时与 python3：

```sh
sudo dnf install qt6-qtbase qt6-qtwayland python3          # Fedora
sudo zypper install qt6-qtbase libQt6WaylandClient6 python3  # openSUSE
```

---

## 2. 从源码构建

```bash
# Fedora
sudo dnf install gcc-c++ make qt6-qtbase-devel qt6-qtwayland python3
# openSUSE Tumbleweed
sudo zypper install gcc-c++ make qt6-qtbase-devel libQt6WaylandClient6 python3

make                 # 编译 build/mc263-qt
make check           # 78 项核心单测 + 7 项内嵌下载器自测（全部离线）
make rpm             # 出 rpm（本分支主线，见第 6 节）
make info            # 编译器 / Qt6 / 产物路径

build/mc263-qt                            # 运行
QT_QPA_PLATFORM=offscreen build/mc263-qt  # 无图形环境冒烟测试
MC263_SHOT=/tmp/ui.png build/mc263-qt     # 抓首屏 PNG 后退出（看布局）
```

编译只链接 `Qt6Widgets / Qt6Gui / Qt6Core`，不需要 moc、不需要 CMake。

---

## 3. 界面与功能

导航是**顶部一排横向标签**（切页带淡入动画），不是侧边栏：

| 页面 | 能力 |
|------|------|
| 启动游戏 | 本地版本下拉、玩家名、一键启动；`QProcess` 异步 exec java，日志实时刷新 |
| 下载游戏 | 在线版本列表（可勾「含预览版」把 snapshot 一起列出来）、组件勾选、official / bmclapi 镜像、并发线程（默认 10）；JSON 进度条与日志；**清除下载缓存**按钮（只删元数据缓存，已下载的 jar / 库 / 资源不受影响） |
| Mod 下载 | 搜 Modrinth：关键词 + 加载器 + 游戏版本过滤，**排序可选下载量 / 相关度 / 关注数 / 最新发布 / 最近更新**；双击即装，SHA1 校验、已存在则跳过；**自动补齐 Mod 的 required 前置**，拿不到文件的转成手动下载清单 |
| 版本管理 | 左栏是已装版本（卡片列表，选中态实心强调色），右栏是**该版本的 Java 与兼容性查验**：扫配置 / Mojang JRE / `JAVA_HOME` / `PATH` / `/usr/lib/jvm`，11 项检查（架构、Java 版本、依赖库、natives、图形音频库、内存、磁盘、路径守卫，可选网络连通性） |
| 加载器 | Fabric / Quilt 版本列表拉取 + 一键安装：抓 profile json、与原版 json 合并、下载依赖到 `libraries/`，生成 `versions/<版本>+<加载器>/` |
| 游戏目录 | 独立一页：自动扫描 ELF 旁 / XDG 数据目录 / `~/.minecraft`（**只列真实存在的目录**），列出各自的版本数与 mods 数，选中即切换，避免两套装各用一份 |
| 设置 / 内存 | **配色来源可选**：自动（找到 Noctalia 色板就用）/ 跟随系统调色板 / Noctalia（Material You）；**内存默认交给 JVM 管理**（不传 `-Xmx`/`-Xms`），关掉自动后可手动拖；四段式内存条：程序占用 → 缓存 → 游戏堆 → 余下可用，2 秒刷新、超可用内存红字提示；额外 JVM 参数；**游戏语言默认跟随系统 locale**（识别不到就 `en_us`） |

窗口圆角默认 12px（`kWindowRadius` 常量，合成器给了服务端装饰时它自己画圆角、这里就不画）。

**版本隔离（始终开启，没有关闭开关）**：启动时 `--gameDir` 指向 `versions/<版本>/`，
`mods`、`saves`、`resourcepacks`、`shaderpacks`、`logs` 都在该目录下自动建好 ——
不同版本（含 `26.3` 与 `26.3+fabric`）互不影响，存档和 Mod 不会串。
Mod 下载也装进对应版本的目录，而不是全局 `minecraft/mods/`。

---

## 4. 启动前的 Java 检查

启动时先扫 Java，达标才放行。

| 版本 | 需要的 Java |
|------|-------------|
| 26.x（主线） | **25+** |
| 1.20 ~ 1.21 | 21 |
| 1.17 ~ 1.19 | 17 |
| 1.16 及更早 |  1.8|

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

启动时按会话选后端：显式设了 `QT_QPA_PLATFORM` 就完全尊重；否则 Wayland 会话优先 `wayland`，
缺插件回退 `xcb`（需 XWayland），两个都没有就打印安装命令后退出，不让 Qt `qFatal`。

| 情形 | 结果 |
|------|------|
| 已设 `QT_QPA_PLATFORM`（或命令行 `-platform`） | 完全尊重，不干预 |
| Wayland 会话 + 装了 wayland 插件 | `wayland`，原生协议，不经过 XWayland |
| Wayland 会话 + 只有 xcb 插件 | 回退 `xcb`（需 XWayland），stderr 提示补装 |
| 两个插件都缺 | 打印 Fedora / openSUSE 的安装命令后退出 |

`setDesktopFileName("mc263-qt")` 让 Wayland 的 `app_id` 与桌面项一致（任务栏分组、图标）。
「关于」页会显示当前后端与 Qt 版本，可直接确认。

---

## 6. 打包成 rpm

### 6.1 本地出包

```bash
sudo dnf install rpm-build gcc-c++ make qt6-qtbase-devel
bash packaging/build-rpm.sh
#   → dist/mc263-26.3-1.fc45.x86_64.rpm   正式包（带发行版后缀）
#   → dist/mc263-26.3-1.x86_64.rpm        稳定名副本（install.sh 用）
#   → dist/mc263-qt-26.3-x86_64           独立 ELF
#   → dist/SHA256SUMS                     所有产物的摘要（离线兜底）
```

不需要 root 跑 rpmbuild（`_topdir` 固定到项目内 `build/rpmbuild`）。常用开关：

- `--release 2`：版本号不变但内容改了，必须 +1，否则 `dnf` 认为是同一个包、不会升级
- `--sign <指纹>`：加 GPG 签名（RPM 6 内置，不需要 `rpm-sign`）；不加就是未签名包，
  Fedora 45 会直接拒装（见 6.4）

### 6.2 发布到 COPR（推荐的签名路线）

COPR 是 Fedora 官方的第三方构建服务：不用审核、自动给各 Fedora 版本构建，
而且**用 Fedora 的密钥签名**——正好解决 45 的签名门槛，用户 `dnf install` 不再需要 `--nogpgcheck`。

COPR 的 v3 API **只收 `.src.rpm` / `.nosrc.rpm` / `.spec`**，所以先把
`mc263-source.spec` 与源码 tarball 装进 src.rpm：

```bash
sudo dnf install copr-cli
bash packaging/make-srcrpm.sh            # → dist/mc263-26.3-1.fc45.src.rpm
copr-cli build cxc123/one-263-linux dist/mc263-26.3-1.fc45.src.rpm
```

逐步操作、项目页文案与失败排查见 [docs/copr-publish.md](docs/copr-publish.md)。
两份 spec 的分工：`packaging/mc263.spec` 塞预编译二进制（本地出包快，用户不必装编译环境）；
COPR 在 mock 里现编源码，用 `packaging/mc263-source.spec`（`%prep` 解包 + `%build` 跑 `make` + `%check` 跑单测）。

### 6.3 自建仓库（最快，不需要账号）

```bash
sudo dnf install createrepo-c
bash packaging/make-repo.sh               # → dist-repo/（含 repomd.xml）
```

把 `dist-repo/` 传到能走 HTTPS 的地方，用户加一个 repo 文件（模板见 `packaging/mc263.repo.example`）：

```bash
sudo cp mc263.repo.example /etc/yum.repos.d/mc263.repo
sudo sed -i 's|__BASEURL__|https://你的域名/mc263/|' /etc/yum.repos.d/mc263.repo
sudo dnf install mc263
```

### 6.4 签名要求（Fedora 45 / RPM 6.1）

Fedora 45 起 RPM 默认 `%_pkgverify_level all`：未签名包不再只是 `NOKEY` 警告，
而是 `package … does not verify` **直接拒绝**。

`install.sh` 遇到这种拒绝**就停下来**，不自动降级：不跳过 GPG 检查、不退化到
`rpm --nosignature`、也不改任何系统级配置。它把三条路打印出来，由你决定：

| 选 | 做法 | 代价 |
|---|---|---|
| 1（推荐） | 用已签名的 COPR 源：`sudo dnf copr enable cxc123/one-263-linux && sudo dnf install mc263` | 需要联网，首次会拉 repodata |
| 2 | 装免安装 ELF：`sh install.sh --elf` | 没有菜单项，要自己准备 Qt6 运行时 |
| 3 | 你确认过来源，只对这一次放宽：`sudo dnf install --nogpgcheck ./mc263-26.3-1.x86_64.rpm` | 这一个包不验签，其它包不受影响 |

> **不要**为了装一个包去改 `/etc/rpm/macros.verify`（或 `--define '%_pkgverify_level digest'`）——
> 那等于给这台机器上**所有** rpm 永久放行，装上什么东西都不会再被拦。

想自己签名（RPM 6 内置，不需要 `rpm-sign`）：

```bash
gpg --quick-generate-key "mc263 <you@example.com>" rsa4096 sign never
bash packaging/build-rpm.sh --sign <密钥指纹或邮箱>
#   → dist/mc263-signing-key.asc（自动导出）
# 用户侧：sudo rpmkeys --import mc263-signing-key.asc
```

`rpm-setup-autosign` 只适合本机构建机自动签名，解决不了别的机器的信任问题。

---

## 7. 目录结构

```
mc26-one/
├── Makefile                    all / check / rpm / arch / info / clean
├── README.md
├── install.sh                  一键安装（rpm / pacman / ELF 自动选择 + 摘要校验）
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
│   └── copr-publish.md         COPR 上传流程与项目页文案
├── packaging/
│   ├── mc263.spec              rpm spec（装预编译二进制）
│   ├── mc263-source.spec       rpm spec（从源码构建，给 COPR / OBS）
│   ├── make-srcrpm.sh          spec + tarball → .src.rpm（COPR 只收这个）
│   ├── build-rpm.sh            make → rpmbuild → dist/*.rpm + 独立 ELF + SHA256SUMS
│   ├── make-repo.sh            用 createrepo_c 生成 dnf 仓库目录
│   ├── mc263.repo.example      用户侧 repo 文件模板
│   ├── mc263.desktop           应用菜单项（Exec=mc263-qt）
│   ├── niri-mc263.kdl          niri 窗口规则示例（浮动尺寸 / 圆角 / 边框）
│   └── mc263.svg               图标
└── tests/
    ├── core_test.cpp           57 项核心单测
    ├── downloader_test.sh      内嵌下载器 + mock CDN 自测（7 项）
    ├── mock_cdn.py             离线模拟 Mojang 下载端点（真实 SHA1）
    └── probe_endpoints.py      真实端点诊断
```

> `packaging/PKGBUILD`、`build-arch.sh`、`make-repo-arch.sh`、`docs/arch-handoff.md`
> 是从 main 合并过来的 Arch 线文件：在这条分支上不维护，也不随 rpm 发布走（打包用不到它们）。

---

## 8. 常见问题

**Q：提示需要 Java 25，但我装了 17？**
A：26.3 要 25（1.20+ 要 21、1.17+ 要 17）。点「下载 Java 运行时」拿 Mojang 官方 25，
或「手动指定 java…」指向更高版本。

**Q：没有 Java 能先下游戏吗？**
A：能。检测不到 Java 时，下载页会自动出现蓝色「Java 运行时」并默认勾选，下完即可启动。

**Q：`qt.qpa.plugin: Could not load the Qt platform plugin "wayland"`？**
A：缺 `qt6-qtwayland`（openSUSE 是 `libQt6WaylandClient6`）。装上即可；
临时也可 `QT_QPA_PLATFORM=xcb build/mc263-qt` 走 XWayland。

**Q：`dnf` 说包没有签名 / 拒绝安装？**
A：Fedora 45 起默认拒绝未签名包。走 COPR 源最省事，或按第 6.4 节里
三条路自己选——脚本不会替你放宽校验。

**Q：下载慢 / 个别文件失败？**
A：下载页把「下载源」切到 `bmclapi`，并发调到 12，再点一次——已存在且 SHA1 一致的文件自动跳过。
`python3 tests/probe_endpoints.py` 可诊断真实端点。

**Q：能登录正版账号吗？**
A：**暂时不能**。启动参数是离线模式（`--accessToken 0 --userType legacy`），只能玩单人 / 局域网。
正版登录需要 Microsoft OAuth → Xbox Live → XSTS → Minecraft Services，尚未实现。

---

## 9. 许可与法律声明

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
