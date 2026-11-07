# mc26-one — Minecraft 26.3 的 Linux 原生启动器（Qt6 / C++23）

> **许可：GPL-3.0-or-later**（全文见 [`LICENSE`](LICENSE)）

一个 HMCL 形态的启动器：**单个 ELF + 应用目录下的 `minecraft/` 与 `.mc263/`**。
没有 Electron、没有内嵌浏览器，Qt6 Widgets 直接画界面；显示后端在启动时按会话自动
选 Wayland 或 XWayland，KDE Plasma 6 这种只给 Wayland 的桌面也能原生跑。

```
mc263-qt                 ← 唯一可执行文件（Qt6 Widgets，无 moc / 无 Q_OBJECT）
minecraft/               ← 游戏目录（首次运行自动建立，也可在设置里改）
.mc263/
├── config.json          版本 / 内存 / Java / 镜像 / 组件（改完即存）
└── mc263_downloader.py  内嵌 python3 下载脚本（运行时释放）
```

> 如果可执行文件所在目录不可写（例如装进 `/usr/bin`），启动器会自动把上面两个目录
> 放到 `~/.local/share/mc263/`，不会去写系统目录。

---

## 0. 一键安装（Fedora / openSUSE）

```sh
curl -fsSL https://raw.githubusercontent.com/zzxwithbus19/one-263-linux/main/install.sh | sh
```

脚本做三件事：从 GitHub Releases 下载**预编译 rpm** → 用 `dnf`/`zypper` 先装 Qt6 运行时
（`qt6-qtbase`、`qt6-qtwayland`、`python3`）→ 再 `sudo dnf install ./mc263-*.rpm` 装本体。
装完 `mc263-qt` 直接跑，或从应用菜单点「Minecraft 26.3」。

> Fedora 45 / RPM 6.1 默认 `%_pkgverify_level all`，未签名的本地包会被硬拒
> （`package … does not verify`）。脚本已自动降级：`dnf install` → `--nogpgcheck` →
> DNF5 的 `--no-gpgchecks` → `rpm -Uvh --nosignature`。原理和签名做法见 [5.1](#51-fedora-45--rpm-61-的签名要求)。
> 若 release 里带了 `SHA256SUMS` / `mc263-signing-key.asc`，脚本会先校验摘要、导入公钥，
> 这样包是「正规安装」而不是靠绕过。摘要优先用 GitHub 自动给的 asset digest，
> 拿不到才退回 `SHA256SUMS`。

卸载：

```sh
sh install.sh --uninstall      # 或 curl ... | sh -s -- --uninstall
sudo dnf remove mc263
```

没有 `dnf` / `zypper`（Arch、Debian…）？脚本会自动退回「独立 ELF」模式：下载
`mc263-qt-26.3-<arch>` 装到 `~/.local/bin`（root 则是 `/usr/local/bin`），不碰包管理器。
强制走这条路用 `sh install.sh --elf`。ELF 需要自己先装 Qt6 运行时和 python3：

```bash
sudo pacman -S qt6-base qt6-wayland python        # Arch
sudo apt install qt6-base-dev python3             # Debian/Ubuntu
```

想自己指定版本 / 源：`MC263_VER=26.4 MC263_REPO=you/fork sh install.sh`。

---

## 1. 能做什么

| 页面 | 能力 |
|------|------|
| 启动游戏 | 本地版本下拉、玩家名、一键启动；`QProcess` 异步 exec java，日志实时刷新 |
| 下载游戏 | 在线版本列表、组件勾选、official / bmclapi 镜像、并发线程（默认 10）；JSON 进度条 + 日志 |
| Java 与兼容性 | 后台线程扫描配置 / Mojang JRE / `JAVA_HOME` / `PATH` / `/usr/lib/jvm`，列出版本与来源；11 项兼容性查验（架构、Java 版本、依赖库、natives、图形音频库、内存、磁盘、路径守卫，可选网络连通性） |
| 设置 / 内存 | 四段式内存条：程序占用（蓝）→ 缓存（浅蓝）→ 游戏 `-Xmx`（浅灰，长度就是分配量）→ 余下可用；2 秒自动刷新，超出可用内存红字提示 OOM；`-Xms`、JVM 参数、游戏目录；**改完即存，没有保存按钮** |
| 关于 | 本地 `README.md` 蓝字链接、仓库链接、当前显示后端与 Qt 版本 |

### 启动前的 Java 闸门

启动器不再让你自己勾「要不要下 Java」：启动时先扫一遍系统，用整页遮住主界面
（不是弹窗，点不穿），达标才放行。

| 版本 | 要求的 Java |
|------|-------------|
| 26.x（本仓库主线） | **25** |
| 1.20 ~ 1.21 | 21 |
| 1.17 ~ 1.19 | 17 |
| 1.12 及更早 | 8 |

阈值不是写死的：优先读已下载的 `<版本>.json` 里 `javaVersion.majorVersion`，没下载时按版本号推断
（`core::requiredJavaMajor`，有单测）。三档文案：

- 只找到 Java 8 / 11 → 「当前 Java 11 已不再被 Minecraft 26.3 所支持」
- 只找到 17 / 21 但不够 → 「当前 Java 21 低于 Minecraft 26.3 要求的 Java 25」
- 一个都没有 → 提示去下载页；那里「Java 运行时」会**蓝色高亮并默认勾选**

卡片上还有「重新检测 / 手动指定 java… / 退出」；下载完或指定后自动重扫，达标即自动放行。
点「启动游戏」时还会按当前选中版本再算一次，不达标就把闸门拉回来。

### Wayland 原生

KDE Plasma 6 起默认只提供 Wayland 会话，XWayland 可能根本没装。启动器在
`QApplication` 之前就按会话选好 Qt 平台插件（`core::choosePlatform`，有离线单测）：

| 情形 | 结果 |
|------|------|
| 已设 `QT_QPA_PLATFORM`（或命令行 `-platform`） | 完全尊重，不干预 |
| Wayland 会话 + 装了 wayland 插件 | `wayland`，原生协议，不经过 XWayland |
| Wayland 会话 + 只有 xcb 插件 | 回退 `xcb`（需 XWayland），stderr 提示补装 |
| 两个插件都缺 | 打印 Fedora / openSUSE 的安装命令后退出，而不是让 Qt qFatal |

配套：`setDesktopFileName("mc263-qt")` 让 Wayland 的 `app_id` 与桌面项一致（窗口分组、任务栏图标）；
HiDPI、分数缩放、多屏交给 Qt6；打开链接走 `xdg-open` / portal。
「关于」页底部会显示 `显示后端：wayland（原生 Wayland，不经过 XWayland）· Qt 6.x.y`。

---

## 2. 环境要求

```bash
# Fedora
sudo dnf install gcc-c++ make qt6-qtbase-devel qt6-qtwayland python3
# openSUSE Tumbleweed
sudo zypper install gcc-c++ make qt6-qtbase-devel libQt6WaylandClient6 python3
```

| 依赖 | 用途 |
|------|------|
| gcc ≥ 15 / make | 编译（C++23：`std::print`、`std::string::contains`） |
| qt6-qtbase-devel | 编译期头文件 + `pkg-config Qt6Widgets` |
| qt6-qtbase / qt6-qtwayland | 运行期：Qt 库与 Wayland 平台插件（只装 base 也能跑，但会退到 XWayland） |
| python3 | 下载器脚本（运行时释放到 `.mc263/`） |

编译只链接 `Qt6Widgets / Qt6Gui / Qt6Core`，**不需要 moc**，产物 400 KB 左右。

---

## 3. 快速开始

```bash
make                 # 编译 build/mc263-qt
make check           # 46 项核心单测 + 7 项内嵌下载器自测（全部离线）
make info            # 编译器 / Qt6 / 产物路径

build/mc263-qt                            # 直接运行
QT_QPA_PLATFORM=offscreen build/mc263-qt  # 无图形环境冒烟测试
```

`make check` 覆盖：路径守卫（`/`、`/home`、`/etc` 等 7 个敏感路径 + 符号链接逃逸）、
配置读写、Java 版本探测、兼容性检查、启动命令拼装、`/proc/meminfo` 三种口径、
Wayland / xcb 后端选择、各版本 Java 阈值；下载器部分会从源码里抽出内嵌 python 脚本，
在离线 mock CDN 上验证 SHA1、并发、进度与落盘。

首次使用：进「下载游戏」页 → 「获取在线版本」→ 选 26.3 → 全勾 → 开始下载（约 700 MB）。
下载完回「启动游戏」页，闸门会自动放行。

---

## 4. 目录结构

```
mc26-one/
├── Makefile                    all / check / rpm / info / clean
├── README.md
├── include/
│   ├── launcher_core.hpp       核心：布局 / 配置 / Java 探测 / 兼容性 / 启动 / meminfo / 后端选择
│   ├── download.hpp            curl 下载后端 + 自实现 SHA1 + 并发池（兼容性网络检查用）
│   ├── mini_json.hpp           JSON 解析与生成
│   ├── mini_zip.hpp            ZIP 读取（扫 natives）
│   ├── safe_path.hpp           路径守卫（黑名单 + 沙箱根 + 符号链接逃逸防护）
│   └── fs_util.hpp             目录 / 文件 / 环境变量小工具
├── src/
│   └── mc263_qt.cpp            Qt6 主窗口（5 页，内嵌 python3 下载脚本）
├── packaging/
│   ├── mc263.spec              rpm spec（装预编译二进制，不要求装 Qt 开发包）
│   ├── build-rpm.sh            make → rpmbuild → dist/*.rpm
│   ├── mc263.desktop           应用菜单项（Exec=mc263-qt）
│   └── mc263.svg               图标
├── install.sh                  一键安装脚本（下载 rpm + dnf/zypper 解依赖）
└── tests/
    ├── core_test.cpp           46 项核心单测
    ├── downloader_test.sh      内嵌下载器 + mock CDN 自测（7 项）
    ├── mock_cdn.py             离线模拟 Mojang 下载端点（真实 SHA1）
    └── probe_endpoints.py      真实端点诊断
```

---

## 5. 打包成 rpm

rpm 里放的是**预编译好的二进制**：打包装包的人需要 Qt6 开发包（编译用），
**安装的人不需要**——`dnf` / `zypper` 会自己把 `qt6-qtbase` 这些运行时依赖装上。
不需要 root 跑 rpmbuild（`_topdir` 指到项目内 `build/rpmbuild`，输出到 `dist/`）。

```bash
# 1) 打包装包机：rpmbuild + 编译依赖
sudo dnf install rpm-build gcc-c++ make qt6-qtbase-devel

# 2) 编译 + 打包
bash packaging/build-rpm.sh
#   → dist/mc263-26.3-1.fc45.x86_64.rpm   （带发行版后缀，正式包）
#   → dist/mc263-26.3-1.x86_64.rpm        （稳定名别名，install.sh 用它）

# 3) 本机试装（dnf 会解析 Qt6 运行时依赖）
sudo dnf install dist/mc263-26.3-1.x86_64.rpm
mc263-qt
sudo dnf remove mc263
```

脚本做的事：`make all` → 把 `build/mc263-qt`、desktop、图标、LICENSE 放进 `SOURCES`
→ `rpmbuild -bb` → 拷到 `dist/`。换版本：`bash packaging/build-rpm.sh --version 26.4`。

包内容：`/usr/bin/mc263-qt`、`/usr/share/applications/mc263.desktop`、hicolor 图标、`LICENSE`。
Qt 的库依赖由 rpm 从 ELF 的 `DT_NEEDED` 自动生成（`libQt6Widgets.so.6` 等），Fedora / openSUSE
都能解析；另外 `Requires: python3`，`Recommends: qt6-qtwayland`（只写在 Fedora 分支里）。
装好后配置与游戏数据落在 `~/.local/share/mc263/`（因为 `/usr/bin` 不可写）。

**发布给 `install.sh` 用**：把 `dist/mc263-26.3-1.fc45.x86_64.rpm` 传到 GitHub Releases
（tag 随意，`latest` 指最新一个），`install.sh` 会先按稳定名
`releases/latest/download/mc263-26.3-1.x86_64.rpm` 取，取不到再用 API 从最新 release 里挑第一个 `.rpm`。

**Release 该传哪些文件**

| 资产 | 用途 | 给谁 |
|------|------|------|
| `mc263-26.3-1.fc45.x86_64.rpm` | 主发布物：装 `/usr/bin` + 菜单项，`dnf`/`zypper` 自动解 Qt6 依赖 | Fedora / openSUSE 用户 |
| `mc263-26.3-1.x86_64.rpm` | 稳定名副本（同字节），`install.sh` 的固定 URL 用它 | 脚本 |
| `mc263-qt-26.3-x86_64` | **独立 ELF**，`chmod +x` 直接跑，不需要安装 | 非 rpm 发行版、免安装党 |
| `SHA256SUMS` | 所有产物的摘要（**可选**：GitHub 会给每个 asset 自动附 sha256 digest，这个只作离线兜底） | 想离线核对的人 |
| `mc263-signing-key.asc` | 公钥（**可选**，不维护 GPG 就不传） | 想验证签名的人 |

独立 ELF 的取舍：零安装、可放 U 盘，但**没有**菜单项和图标（要就装 rpm），
且必须自备 `qt6-qtbase`、`qt6-qtwayland`（原生 Wayland 用）和 `python3`；
升级要自己换文件。rpm 则相反：依赖自动装、菜单/卸载干净，代价是 Fedora 45 的签名门槛
（不签名就走 `--nogpgcheck`，见 5.1）。

### 5.1 Fedora 45 / RPM 6.1 的签名要求

Fedora 45 起 RPM 的默认验证级别从 `digest` 改成 `all`：未签名包不再只是 `NOKEY` 警告，
而是 `package mc263-… does not verify` **直接拒绝安装**（跟随上游 RPM 6.0+ 的默认行为）。
三条路，按推荐顺序：

**1）让脚本处理（最省事）**

`install.sh` 已经是四级降级：`dnf install` → `dnf --nogpgcheck` → `dnf --no-gpgchecks`
（DNF5 对本地包的回归，issue #2614）→ `rpm -Uvh --nosignature`。手动等价：

```bash
sudo dnf install --nogpgcheck ./mc263-26.3-1.x86_64.rpm
sudo dnf install --no-gpgchecks ./mc263-26.3-1.x86_64.rpm        # DNF5
sudo rpm --define '_pkgverify_level digest' -Uvh --nosignature ./mc263-26.3-1.x86_64.rpm
```

想永久放宽（自编译包多的话）：

```bash
echo '%_pkgverify_level digest' | sudo tee /etc/rpm/macros.verify
# 彻底关掉完整性校验（不推荐）：改成 none
```

**2）自己签名（正式分发推荐）**

RPM 6 自带 OpenPGP 签名，不需要 `rpm-sign`：

```bash
gpg --quick-generate-key "mc263 <you@example.com>" rsa4096 sign never
bash packaging/build-rpm.sh --sign 145DDD712B37C842   # 指纹/邮箱都行
#   → dist/mc263-26.3-1.x86_64.rpm（已签名）
#   → dist/mc263-signing-key.asc（脚本自动导出的公钥）
```

用户侧导入公钥后，`dnf install` 不再需要任何绕过；`install.sh` 也会自动尝试
从 release 里下载 `mc263-signing-key.asc` 并 `rpmkeys --import`：

```bash
sudo rpmkeys --import mc263-signing-key.asc
sudo dnf install ./mc263-26.3-1.x86_64.rpm
```

> 私钥别跟着 release 走。发布前把 `build/gnupg`（或你自己的 `~/.gnupg`）备份到安全位置，
> 公钥可以随便发。测试用的一次性密钥可以直接删掉重建。

**3）构建机自动签名**

`/usr/lib/rpm/rpm-setup-autosign` 能让本机 rpmbuild 自动签名，省掉每次手动 `rpmsign`；
但它生成的密钥只在**本机** gpg 库里，别的机器不认——所以对外分发仍要走第 2 条（导出公钥让别人 import）。

**4）完全不想碰密钥：用摘要校验**

GitHub 会给每个 release asset 自动算一个 `sha256:` digest（Release 页面和 API 的
`assets[].digest` 都能看到）。`install.sh` 优先拿它比对，不用你上传任何东西：

```sh
# 脚本内部等价于：
curl -fsSL https://api.github.com/repos/zzxwithbus19/one-263-linux/releases/latest \
  | python3 -c 'import json,sys; ...'   # 取 assets[].digest
sha256sum mc263-26.3-1.x86_64.rpm       # 与 digest 对比
```

`dist/SHA256SUMS` 仍然会生成，但只是**离线兜底**（API 不通 / 想在没有网络的环境核对时用）：

```bash
cd dist && sha256sum -c SHA256SUMS
```

CI 里也常用这招：`rpm -qp --qf '%{OPENPGP:pgpsig}\n'` 打印签名者与 Key ID 做记录，
再用 digest 或 `sha256sum -c` 卡住产物。

---

## 6. 常见问题

**Q：启动器提示需要 Java 25，但我装了 17？**
A：26.3 就是要 25（1.20+ 要 21、1.17+ 要 17）。点「下载 Java 运行时」拿 Mojang 官方 25，
或者「手动指定 java…」指向本机更高版本。

**Q：没有 Java 又想先下游戏？**
A：可以。下载页的「Java 运行时」在检测不到 Java 时会自动出现并默认勾选，下完就能启动。

**Q：Wayland 下窗口没图标 / 任务栏分组不对？**
A：从 rpm 安装（有 `mc263-qt.desktop`）就正常；源码直接跑 `build/mc263-qt` 时系统里没有同名桌面项，
不影响功能，只影响图标归属。

**Q：`qt.qpa.plugin: Could not load the Qt platform plugin "wayland"`？**
A：没装 `qt6-qtwayland`（openSUSE 是 `libQt6WaylandClient6`）。装上重启即可；
临时也可以 `QT_QPA_PLATFORM=xcb build/mc263-qt` 走 XWayland。

**Q：内存条里「缓存」和「游戏 -Xmx」是什么关系？**
A：缓存是文件页缓存，可回收；灰块长度就是分给 JVM 的 `-Xmx`，从缓存之后开始画，
右边的轨道是启动后仍可用的余量。设太大时会先挤掉缓存，再不够就红字提示 OOM 风险。

**Q：下载慢 / 个别文件失败？**
A：下载页把「下载源」切到 `bmclapi`，并发调到 12，再点一次「开始下载」——已存在且 SHA1
一致的文件会自动跳过。`python3 tests/probe_endpoints.py` 可以诊断真实端点。

**Q：能登录正版账号吗？**
A：**暂时不能**。启动参数是离线模式（`--accessToken 0 --userType legacy`），只能玩单人 / 局域网。
正版登录需要接 Microsoft OAuth → Xbox Live → XSTS → Minecraft Services，还要处理 token 刷新，
尚未实现。

---

## 7. 许可与法律声明

### 7.1 许可

本项目源码采用 **GPL-3.0-or-later**。历史沿革：上游 `zhanbalao/mc26-one` 未声明许可证，
且其唯一的 `build_262.py` 已从本仓库移除；当前代码为全新 C++23 实现，不含上游代码。

Qt6 为 `LGPL-3.0-only OR GPL-3.0-only WITH Qt-GPL-exception-1.0`，本项目按 **LGPL-3.0-only**
动态链接使用：LGPL 与 GPLv3 兼容，项目许可不变。分发二进制时需随附 LGPL-3.0 文本、
声明使用了 Qt、保持动态链接以便替换 Qt 库，并提供 Qt 源码获取途径。
`mc263.spec` 的 `License` 字段为 `GPL-3.0-or-later`（不含 Qt 代码本体）。

### 7.2 第三方素材

本仓库**不包含** Minecraft 游戏文件、Java 运行时或任何 Mojang / Microsoft 版权素材。
启动器的下载器只从 Mojang 官方 CDN 或公共镜像获取公开资源文件，
**不提供账号认证、不绕过任何验证**；运行游戏需要你自己拥有正版 Minecraft。
下载到本地的游戏文件、Mojang Java 运行时与第三方库各自适用其原有许可。
