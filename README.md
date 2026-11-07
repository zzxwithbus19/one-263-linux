# mc263 — Minecraft 26.3 Linux 启动器（Qt6 / C++23）

> 许可：GPL-3.0-or-later（全文见 [LICENSE](LICENSE)）
>
> 分支：`fedora`（本分支）= RHEL / Fedora 的 rpm 线（rpmbuild + COPR）；
> `main` = Arch 主线（PKGBUILD + pacman 自建源）。两条线的打包产物互不冲突，
> 源码（`src/` `include/`）两边同步。

HMCL 形态的启动器：**一个 ELF + 旁边的 `minecraft/` 与 `.mc263/`**。
Qt6 Widgets 原生界面（无 Electron、无内嵌浏览器、无 moc），启动时自动选择
Wayland / XWayland，KDE Plasma 6 这类只给 Wayland 的桌面也能原生运行。

```
mc263-qt                 ← 唯一可执行文件（约 400 KB）
minecraft/               ← 游戏目录（首次运行自动建，可在设置里改）
.mc263/
├── config.json          版本 / 内存 / Java / 镜像 / 组件（改完即存）
└── mc263_downloader.py  内嵌 python3 下载脚本（运行时释放）
```

> 装在 `/usr/bin` 这类只读位置时，这两个目录自动改放 `~/.local/share/mc263/`。

---

## 1. 安装

#### 见发布页

### 手动下载

| 发行版 / 方式 | 文件 | 做法 |
|---|---|---|
| Fedora / openSUSE | `mc263-26.3-1.<arch>.rpm` | `sudo dnf install ./mc263-26.3-1.x86_64.rpm` |
| Arch | `mc263-26.3-1-x86_64.pkg.tar.zst` | `sudo pacman -U ./mc263-26.3-1-x86_64.pkg.tar.zst` |
| 其它 / 免安装 | `mc263-qt-26.3-<arch>` | `chmod +x` 后直接跑 |

Fedora 也可以直接用 COPR 源（自动签名，`dnf` 会验签）：

```sh
sudo dnf copr enable cxc123/one-263-linux
sudo dnf install mc263
```

独立 ELF 没有菜单项和图标，需要自己准备 Qt6 运行时与 python3：

```sh
sudo pacman -S qt6-base qt6-wayland python       # Arch
sudo apt install qt6-base-dev python3            # Debian / Ubuntu
```

---

## 2. 从源码构建

```bash
# Fedora
sudo dnf install gcc-c++ make qt6-qtbase-devel qt6-qtwayland python3
# openSUSE Tumbleweed
sudo zypper install gcc-c++ make qt6-qtbase-devel libQt6WaylandClient6 python3
# Arch
sudo pacman -S --needed base-devel qt6-base qt6-wayland python curl

make                 # 编译 build/mc263-qt
make check           # 57 项核心单测 + 7 项内嵌下载器自测（全部离线）
make info            # 编译器 / Qt6 / 产物路径
make rpm             # 出 rpm（本分支主线）
make arch            # 出 Arch 包（调 packaging/build-arch.sh，需 makepkg）

build/mc263-qt                            # 运行
QT_QPA_PLATFORM=offscreen build/mc263-qt  # 无图形环境冒烟测试
```

编译只链接 `Qt6Widgets / Qt6Gui / Qt6Core`，不需要 moc。

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

## 6. 打包成 rpm

> 当前**主维护目标是 Arch Linux**：`packaging/PKGBUILD` 是主线模板，完整交接说明见
> [docs/arch-handoff.md](docs/arch-handoff.md)。
> 下面的 rpm 路线（预编译 spec / 源码 spec / COPR）**保留可用，但不再同步维护**，只当参考。

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

**Release 该传哪些**

| 资产 | 用途 |
|------|------|
| `mc263-26.3-1.fc45.x86_64.rpm` | 主发布物，装 `/usr/bin` + 菜单项，依赖自动拉 |
| `mc263-26.3-1.x86_64.rpm` | 稳定名副本，`install.sh` 的固定 URL |
| `mc263-qt-26.3-x86_64` | 独立 ELF，免安装；无菜单项、需自备 Qt6 与 python3 |
| `SHA256SUMS`（可选） | 离线核对；GitHub 自带 digest 时可不传 |
| `mc263-signing-key.asc`（可选） | 公钥，签名验证用 |

### 6.1 发布到 dnf 仓库

**A. COPR（推荐，免费 + 官方签名）**

> 项目页与逐步操作（描述文案、构建命令、失败排查）见 [docs/copr-publish.md](docs/copr-publish.md)。

COPR 是 Fedora 官方的第三方构建服务：不用审核、自动给各 Fedora 版本构建，
而且**用 Fedora 的密钥签名**——正好解决 45 的签名门槛，用户 `dnf install` 不再需要 `--nogpgcheck`。

```bash
sudo dnf install copr-cli
bash packaging/make-srcrpm.sh            # → dist/mc263-26.3-1.fc45.src.rpm
copr-cli create cxc123/one-263-linux     # 建一次即可（网页建也行）
copr-cli build cxc123/one-263-linux dist/mc263-26.3-1.fc45.src.rpm
```

用户侧：

```bash
sudo dnf install dnf-plugins-core
sudo dnf copr enable cxc123/one-263-linux
sudo dnf install mc263
```

注意两份 spec 的分工：`packaging/mc263.spec` 是**塞预编译二进制**的（本地出包快，用户不必装编译环境）；
COPR 在 mock 里现编源码，用 `packaging/mc263-source.spec`（`%prep` 解包 + `%build` 跑 `make` + `%check` 跑单测）。
COPR 的 v3 API **只收 `.src.rpm` / `.nosrc.rpm` / `.spec`**，所以由 `make-srcrpm.sh` 把 spec 与 tarball
装进 src.rpm 再上传（直接传 tarball 会报 `You can upload only .src.rpm, .nosrc.rpm, and .spec files`）。

**B. 自建仓库（最快，不需要账号）**

```bash
sudo dnf install createrepo-c
bash packaging/make-repo.sh               # → dist-repo/（含 repomd.xml）
```

把 `dist-repo/` 传到能走 HTTPS 的地方（自己的 nginx / GitHub Pages / 对象存储），用户加一个 repo 文件
（模板见 `packaging/mc263.repo.example`）：

```bash
sudo cp mc263.repo.example /etc/yum.repos.d/mc263.repo
sudo sed -i 's|__BASEURL__|https://你的域名/mc263/|' /etc/yum.repos.d/mc263.repo
sudo dnf install mc263
```

想连签名一起做：把公钥一并发布，用户 `sudo rpm --import` 后把 repo 文件里的 `gpgcheck=1` 打开。

**COPR 建项目时的选项怎么勾**（创建项目页面最下面那块）：

| 选项 | 建议 | 原因 |
|------|------|------|
| Create repositories manually | **不勾** | COPR 会自动为每个 Fedora 版本建仓库 |
| Enable internet access during builds | **不勾** | 编译与单测全离线（下载器自测跑本地 mock CDN），不需要联网 |
| Project will not be listed on home page | **不勾** | 勾了才不上首页；不勾更容易被搜到 |
| Follow Fedora branching | **勾上** | Fedora 分支出新版本时自动建对应 chroot，不用手动补构建 |
| Multilib support | 不勾 | 只发 x86_64（aarch64 以后再说） |
| This repository contains module hotfixes | 不勾 | 跟模块流无关 |
| Run fedora-review tool for packages in this project | **勾上** | 提交前自动跑打包规范检查（rpmlint 等），能挡掉一堆低级问题 |
| Generate AppStream metadata | **勾上** | 配合包里的 `mc263.metainfo.xml`，GNOME Software / KDE Discover 能显示图标、简介与更新提示 |
| Delete after days | 留空 | 不自动删项目 |
| Repo priority | 留默认 | |
| Build isolation / Mock bootstrap | 留默认（mock-core-configs） | |
| Runtime dependencies | 留空 | 依赖全在 Fedora 官方仓库里 |
| Packit allowed forge | 留空 | |

> 勾上 fedora-review 后，规范检查可能会挑出这几处：changelog 邮箱格式（已改成
> `mc263@users.noreply.github.com`）、缺 `BuildArch`（源码 spec 已加 `%{?_arch}`，
> 否则会被当成 noarch，构建必失败）、包内多余文件（`%files` 漏了 metainfo，已补）。

**C. 其他渠道**

- openSUSE：OBS（build.opensuse.org）流程类似，spec 可直接复用
- Flatpak / Flathub：另一条分发线，和 dnf 无关
- 官方 Fedora 源：需要 Fedora Account 的 packager 权限 + 走 Fedora 的审核流程，个人项目基本不现实

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
│   ├── arch-handoff.md         Arch Linux 打包与分发交接文档
│   └── copr-publish.md         COPR 上传与项目页文案
├── packaging/
│   ├── mc263.spec              rpm spec（装预编译二进制）
│   ├── mc263-source.spec       rpm spec（从源码构建，给 COPR / OBS）
│   ├── make-source.sh          生成源码包 dist/mc263-<版本>.tar.gz
│   ├── make-repo.sh            用 createrepo_c 生成 dnf 仓库目录
│   ├── mc263.repo.example      用户侧 repo 文件模板
│   ├── PKGBUILD                Arch 打包模板（main 分支主线）
│   ├── build-arch.sh           源码包 → PKGBUILD → makepkg → 校验 → 冒烟
│   ├── make-repo-arch.sh       用 repo-add 生成自建 pacman 仓库
│   ├── mc263.metainfo.xml      AppStream 元数据（GNOME Software / KDE Discover）
│   ├── make-srcrpm.sh          把 spec + tarball 装进 src.rpm（COPR 只收这个）
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
