# Arch Linux 打包与分发 · 交接文档

> 面向：接手 mc263 在 **Arch Linux** 上的打包与分发（PKGBUILD / pacman / AUR）的你。
> 背景：项目原先以 Fedora 为主（rpm + COPR），现在**收敛到单一发行版（Arch）**，
> Fedora 侧的产物保留但降级为「可选参考」，不再作为主线维护。
> 写这份文档时，仓库状态：版本 26.3，界面 6 页，核心单测 57 项 + 下载器自测 7 项。

---

## 1. 五分钟速览

| 项目 | 值 |
|------|-----|
| 仓库 | https://github.com/zzxwithbus19/one-263-linux |
| 形态 | **单个 ELF**（`mc263-qt`，约 430 KB）+ 同级 `minecraft/` 与 `.mc263/` |
| 技术栈 | C++23 + Qt6 Widgets，**不用 moc / 不用 CMake**，只依赖 `pkg-config(Qt6Widgets)` |
| 构建 | `make` → `build/mc263-qt` |
| 测试 | `make check` → 57 项核心单测 + 7 项下载器自测，**全部离线** |
| 目标系统 | Arch Linux（x86_64），Wayland 优先 |
| 依赖 | `qt6-base`（Qt6Widgets/Gui/Core）、`qt6-wayland`（原生 Wayland 插件）、`python`、`curl` |
| 用户数据 | `~/.local/share/mc263/`（`minecraft/`、`.mc263/config.json`、内嵌下载器脚本） |

代码分层（改代码前先看这里）：

```
include/launcher_core.hpp   平台无关核心：布局 / 配置 / Java 探测 / 兼容性 / 启动计划 / 版本隔离 / 加载器 / 语言
include/modrinth.hpp        Modrinth 搜索 / 版本 / 下载
include/download.hpp        curl 下载后端 + SHA1 + 并发池
include/mini_json.hpp       JSON 解析与生成
src/mc263_qt.cpp            全部界面（6 页）+ QSS 主题 + 后台线程回主线程
tests/core_test.cpp         核心单测（不依赖 Qt）
tests/downloader_test.sh    内嵌 python3 下载器 + mock CDN 自测
packaging/                  打包相关（见下文）
```

---

## 2. 现在已经做完的功能（不用重做）

1. **启动游戏页**：本地版本下拉、玩家名、异步 exec java、实时日志
2. **下载游戏页**：Mojang 官方 CDN / bmclapi 镜像、多线程（默认 10）、SHA1 校验、断点续传、**清除下载缓存**按钮
3. **Mod 下载页**：Modrinth 搜索（关键词 + 加载器 + 游戏版本过滤，排序可选下载量/相关度/关注/最新/更新）、双击即装进 `versions/<版本>/mods/`
4. **加载器**：Fabric / Quilt 版本列表拉取 + 一键安装（抓 profile json → 与原版 json 合并 → 下载依赖 → 生成 `versions/<版本>+<加载器>/`）
5. **Java 与兼容性页**：后台扫描系统 Java / Mojang JRE，11 项兼容性查验
6. **游戏目录页**：自动扫描 ELF 旁 / XDG 数据目录 / `~/.minecraft`（**只列真实存在的目录**，正在使用的目录除外——首次运行可能还没建），检测重复目录并切换
7. **设置 / 内存页**：**内存默认交给 JVM 管理**（不传 `-Xmx`/`-Xms`）、四段式内存条、额外 JVM 参数、**游戏语言跟随系统 locale**（识别不到退回 `en_us`）
8. **版本隔离**（强制）：每个版本用自己的 gameDir，存档 / Mod / 资源包互不干扰

关键设计约定（别破坏）：

- **启动时按会话选显示后端**：`core::choosePlatform()` 在 `QApplication` 之前决定 `wayland` / `xcb`；用户显式设了 `QT_QPA_PLATFORM` 就绝不干预
- **版本隔离不可关闭**：`core::isolatedGameDir()` = `versions/<版本>/`，启动时自动建 `mods/saves/resourcepacks/shaderpacks/logs`
- **游戏语言**：`core::detectSystemLanguage()` 依次看 `LC_ALL → LC_MESSAGES → LANG → LANGUAGE`，映射到 Minecraft 语言码（如 `zh_CN.UTF-8 → zh_cn`），识别不到用 `en_us`
- **下载全部走系统 curl**（`download.hpp`），不引入 libcurl 依赖

---

## 3. 两条分发线：Fedora 降级为参考，Arch 是主线

| 文件 | 用途 | 现在的定位 |
|------|------|-----------|
| `packaging/mc263.spec` | 塞**预编译二进制**的 rpm（本地出包快） | 参考 |
| `packaging/mc263-source.spec` | **从源码构建**的 rpm（COPR / OBS 用） | 参考 |
| `packaging/build-rpm.sh` | make → rpmbuild → `dist/*.rpm` + 独立 ELF + SHA256SUMS | 参考 |
| `packaging/make-source.sh` | 生成 `dist/mc263-<版本>.tar.gz`（源码 + spec），并打印 sha256 | **Arch 也要用**（PKGBUILD 的 source） |
| `packaging/make-repo.sh` | `createrepo_c` 生成 dnf 仓库目录 | 参考（Arch 用 `repo-add`） |
| `packaging/mc263.metainfo.xml` | AppStream 元数据（GNOME Software / KDE Discover） | **直接复用**，Arch 同样吃这个 |
| `packaging/mc263.desktop` / `mc263.svg` | 桌面项与图标 | **直接复用** |
| `packaging/PKGBUILD` | **Arch 打包模板（本次新增）** | 主线 |

> 如果只做 Arch，建议在 README 里把 rpm / COPR 一节标注为「可选」，避免两边同时维护 spec 漂移。

---

## 4. Arch 打包步骤（照着做）

### 4.1 依赖映射

| 项目里用到 | Arch 包名 | 说明 |
|------------|-----------|------|
| Qt6 Widgets/Gui/Core | `qt6-base` | 提供 `pkg-config(Qt6Widgets)` |
| Wayland 平台插件 | `qt6-wayland` | 不装会回退 XWayland（KDE Plasma 6 可能没有 XWayland） |
| 内嵌下载器 | `python` | 运行时释放 `mc263_downloader.py` 并调用 |
| 下载后端 | `curl` | `download.hpp` 直接 exec 系统 curl |
| 游戏运行时 | `java-runtime`（可选依赖） | 启动器自带 Java 探测；不装也能用 Mojang 运行时 |

开发机：`sudo pacman -S --needed base-devel qt6-base qt6-wayland python curl`

### 4.2 生成源码包

```bash
bash packaging/make-source.sh
# → dist/mc263-26.3.tar.gz（含 mc263-26.3/ 目录，包根有 mc263.spec）
#   并打印 sha256，填进 PKGBUILD 的 sha256sums
```

### 4.3 PKGBUILD 要点

仓库里已放好 `packaging/PKGBUILD` 模板，注意这几处：

- `source=("mc263-26.3.tar.gz")` —— 包名必须与 tarball 里的顶层目录 `mc263-26.3/` 一致
- `build()` 里跑 `make`（源码树自带 Makefile，不需要 CMake）
- `package()` 里装五样东西：`/usr/bin/mc263-qt`、`/usr/share/applications/mc263.desktop`、`/usr/share/icons/hicolor/scalable/apps/mc263.svg`、`/usr/share/licenses/mc263/LICENSE`、`/usr/share/metainfo/mc263.metainfo.xml`
- `depends=('qt6-base' 'qt6-wayland' 'python' 'curl')`
- `optdepends=('java-runtime: run the game')`
- **不要**把 `dist/`、`build/`、`.workbuddy/`、`.Trash-*` 打进包（`make-source.sh` 已排除）

### 4.4 本地构建与安装

```bash
cp packaging/PKGBUILD .
# 把 source 换成 dist/mc263-26.3.tar.gz（或软链），填好 sha256sums
makepkg -si          # 构建 + 安装 + 装完自动跑
mc263-qt             # 启动
```

冒烟测试（无显示环境）：

```bash
QT_QPA_PLATFORM=offscreen timeout 3 mc263-qt    # 退出码 124 = 正常跑着
```

### 4.5 两条发布路线

**A. 自建 pacman 仓库**（对应 Fedora 侧的自建 dnf 仓库）

```bash
sudo pacman -S repo-add            # createrepo_c 的等价物
repo-add /var/www/html/mc263/mc263.db.tar.zst *.pkg.tar.zst
```

用户侧 `/etc/pacman.d/mc263.conf`：

```ini
[mc263]
SigLevel = Optional TrustAll
Server = https://你的域名/mc263/$arch
```

**B. AUR**（最省事，用户自己编译）

```bash
makepkg --printsrcinfo > .SRCINFO
# 把 PKGBUILD + .SRCINFO 提交到 AUR 仓库
```

AUR 的审核规则：**PKGBUILD 必须从源码构建**（不能塞预编译二进制），这正好和 `packaging/mc263-source.spec` 的思路一致。

---

## 5. Arch vs Fedora 差异对照（只看会踩坑的）

| 事项 | Fedora | Arch | 对本项目的影响 |
|------|--------|------|----------------|
| 包名 | `qt6-qtbase` / `qt6-qtwayland` | `qt6-base` / `qt6-wayland` | PKGBUILD 里名字不同 |
| 签名门槛 | Fedora 45 起 RPM 默认 `%_pkgverify_level all`，未签名包被硬拒 | pacman 仓库签名是**仓库级**（`SigLevel`），包本身不强制 | Arch 侧**没有**这个坑，README 里 Fedora 章节可标注为 Fedora 专属 |
| 打包工具 | rpmbuild + spec | makepkg + PKGBUILD | 两套并存，别混 |
| 元数据 | RPM + AppStream | PKGBUILD + 同样的 AppStream | `mc263.metainfo.xml` 直接复用 |
| 用户目录 | `~/.local/share/mc263/`（XDG） | 同 | **不用改** |
| 桌面项 | `/usr/share/applications/` | 同 | 不用改 |
| 卸载 | `dnf remove mc263` | `pacman -R mc263` | `install.sh` 需要加分支（见 6.3） |
| 发行版分支 | `fc45` 等 dist tag | 无（滚动发行） | 版本号策略不同：Arch 通常用 `pkgrel` 递增即可 |

---

## 6. 已知的坑（从 Fedora 那边带过来的）

### 6.1 预编译二进制 vs 源码构建

- Fedora 侧的 `mc263.spec` 塞的是预编译 ELF（用户不用装编译环境，出包快）
- **Arch / AUR 必须源码构建**（审核规则），所以用 `make-source.sh` 的 tarball + PKGBUILD 里的 `make`
- 别把两套思路混在一起：Arch 侧永远从源码编

### 6.2 元数据与文件清单

- `mc263.metainfo.xml` 已在两份 spec 里安装，PKGBUILD 也要装到 `%{_datadir}/metainfo/`（`/usr/share/metainfo/mc263.metainfo.xml`）
- 别漏 `LICENSE`（`/usr/share/licenses/mc263/LICENSE`），Arch 的 `makepkg -L` / lint 会挑
- `.desktop` 的 `Exec=mc263-qt`、`Icon=mc263`、`StartupWMClass=mc263-qt` 已配好，直接用

### 6.3 `install.sh` 目前对 Arch 的态度

- `install.sh` 只认 `dnf` / `zypper`，**Arch 上会直接走独立 ELF 分支**（装到 `~/.local/bin`），这是有意为之的降级，不会坏
- 若想让 Arch 用户一条命令用 pacman 装，建议加一个分支（思路）：
  ```sh
  elif command -v pacman >/dev/null 2>&1; then
      PM=pacman   # 下载 pkg.tar.zst 后 sudo pacman -U --noconfirm 包
  ```
  前提是 release 里上传 `.pkg.tar.zst`（COPR/rpm 那套不适用）
- 未签名包的 `--nogpgcheck` 降级链路是 Fedora 专属，Arch 上不存在

### 6.4 本地验证环境的两个坑

- **沙箱里 `rpmbuild` 的 `%prep` 会失败**：`tar` 解包时报 `无法 utime: 不允许的操作`，导致 `%prep` 退出码非零。这是执行环境的限制，不是 spec 的问题（`makepkg` 走的是另一套解包逻辑，通常正常）
- **工作树里可能有 `.Trash-0/`**（某些工具的回收站，可能有几十 MB）：`make-source.sh` 已排除它，但别忘了 `.gitignore` 里也要有

### 6.5 别破坏的三个行为

1. 显式 `QT_QPA_PLATFORM` 必须优先于自动选择
2. 版本隔离没有开关，不许加开关
3. 启动参数里的 `--language` 缺省取系统 locale，取不到 `en_us`

---

## 7. 验收清单（交完做一遍）

```bash
# 1) 编译与测试
make && make check                     # 期望：57 项 + 7 项全过

# 2) 打包与安装
makepkg -si && pacman -Ql mc263        # 期望：五个文件都在

# 3) 冒烟
QT_QPA_PLATFORM=offscreen timeout 3 mc263-qt; echo $?   # 期望 124

# 4) 功能抽查（图形界面里点一遍）
#    - 启动页：Java 不达标会被闸门拦住
#    - 下载页：能拉到版本列表；「清除下载缓存」有确认框
#    - Mod 页：能搜到结果并安装进 versions/26.3/mods/
#    - 游戏目录页：能扫出候选目录并切换
#    - 设置页：内存默认自动（不传 -Xmx）；游戏语言显示「跟随系统（zh_cn）」

# 5) 版本隔离生效
ls ~/.local/share/mc263/versions/26.3/     # 期望有 mods/ saves/ logs/ 等
```

---

## 8. 建议的下一步（按优先级）

1. **落地 PKGBUILD**：把 `packaging/PKGBUILD` 改通，`makepkg -si` 跑通上面第 2、3 条
2. **install.sh 加 pacman 分支**：让 Arch 用户也能一条命令装（release 里放 `.pkg.tar.zst`）
3. **CI**：GitHub Actions 里加一个 job 用 `archlinux` 容器跑 `makepkg`（`pacman -S --needed --noconfirm base-devel qt6-base qt6-wayland python curl`），产出 `.pkg.tar.zst` 作为 artifact
4. **发布流程**：Release 里同时放 `.pkg.tar.zst`、`mc263-qt-<ver>-<arch>`（免安装 ELF）、`SHA256SUMS`
5. **README 调整**：把「一键安装」段改成 Arch 版（pacman 仓库或 AUR），Fedora/rpm 内容折叠成「其他发行版」小节
6. **Fedora 侧冻结**：`packaging/*.spec` 保留现状不再同步，除非明确需要

---

## 9. 协作约定

- 提交信息沿用仓库现有风格（`git log` 里是 `up4`、`up4rc.1` 这类简短写法）
- 改完功能请同步更新 `README.md`，并跑一遍 `make check`
- 项目上下文与历史决策记在 `.workbuddy/memory/*.md`（按日期分文件），接手前先翻一眼
- 改动涉及 `include/launcher_core.hpp` 时记得同步 `tests/core_test.cpp`（核心逻辑改动必须带单测）
