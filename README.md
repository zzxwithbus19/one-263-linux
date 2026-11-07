# mc263 — Minecraft 26.3 Linux 启动器（Qt6 / C++23）

> 许可：GPL-3.0-or-later（ [LICENSE](LICENSE)）

包形态**一个 ELF + 旁边的 `minecraft/` 与 `.mc263/`**。
Qt6 Widgets 原生界面（无 Electron、无内嵌浏览器、无 moc），启动时自动选择
Wayland 原生运行。

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

主线是 **Arch Linux（理论上pacman能用的都可以）和Fedora（理论上任何rpm支持的都可以）**。AUR 上没有（我没账户啊啊啊），copr倒是有

### 1.1 

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

### 1.2 免安装 ELF

`mc263-qt-26.3-x86_64` 是单独一个二进制，没有菜单项和图标，运行时自己备：

```sh
sudo pacman -S --needed qt6-base qt6-wayland  
chmod +x mc263-qt-26.3-x86_64
./mc263-qt-26.3-x86_64
```

> 自备了 Qt6 运行时的其它发行版也能这么跑；但它们目前**不提供打包支持**，自己干去（记得开源或者不商用）
> Fedora / openSUSE 请用 rpm（详见copr）。

---

## 2. 从源码构建

不支持在README.md里提供！不推荐二次分发

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

KDE Plasma 6.8 起默认只有 Wayland 会话。

「关于」页会显示当前后端与 Qt 版本，可直接确认。

---

## 6. Fedora 45 / RPM 6.1 的签名要求

我有COPR

## 7. 目录结构

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

## 8. 常见问题


**Q：没有 Java 能先下游戏吗？**
A：能。检测不到 Java 时，下载页会自动出现蓝色「Java 运行时」并默认勾选，下完即可启动。


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
下载到本地的游戏文件、Mojang Java 运行时与第三方库各自适用其原有许可。
