# Debian / Ubuntu 打包与分发 · 交接文档

> 面向：接手 mc263 在 **Debian 系**（Ubuntu 26.04 LTS / Debian 13+）上打包与分发的人。
> 现状：`main` 分支已经带上 `packaging/build-deb.sh`，`install.sh` 也有 `--deb` 分支；
> rpm 线在 `fedora` 分支（见 `docs/copr-publish.md`），Arch 线见 `docs/arch-handoff.md`。
> 本机实测环境：Ubuntu 26.04.1 LTS (resolute)，Qt 6.10.2、glibc 2.43、dpkg 1.23.7、
> python3 3.14、curl 8.18。版本 26.3，界面 6 页，核心单测 81 项 + 下载器自测 7 项。

---

## 1. 五分钟速览

| 项目 | 值 |
|------|-----|
| 包名 | `mc263`（与 Arch / rpm 一致） |
| 形态 | 单个 ELF（`/usr/bin/mc263-qt`）+ desktop + 图标 + AppStream + doc |
| 构建 | `make`（不使用 CMake / autotools / moc） |
| 出包 | `bash packaging/build-deb.sh`（或 `make deb`）→ `dist/deb/mc263_26.3-1_amd64.deb` |
| 安装 | `sudo apt install ./dist/deb/mc263_26.3-1_amd64.deb` |
| 数据 | `~/.local/share/mc263/`（`minecraft/`、`.mc263/config.json`） |
| 支持范围 | **Ubuntu 26.04+ / Debian 13+**（需要 Qt 6.6+ 的 `QPalette::Accent`） |

## 2. 依赖映射

| 用途 | Ubuntu 26.04 包名 | 说明 |
|------|-------------------|------|
| 编译 | `build-essential`、`qt6-base-dev` | 提供 g++/make 与 `pkg-config(Qt6Widgets)` |
| 运行 | `libqt6core6t64`（旧版本是 `libqt6core6`） | Qt6 Core；control 里写成 `libqt6core6t64 \| libqt6core6` |
| 运行 | `libqt6gui6`、`libqt6widgets6`、`libqt6dbus6` | 二进制直接链接的 Qt 库（可用 `ldd build/mc263-qt` 复核） |
| 运行 | `libqt6waylandclient6` | 提供 `libqwayland.so` 平台插件（**不是** `qt6-wayland` 元包） |
| 运行 | `python3` | 内嵌下载器 `mc263_downloader.py`（多线程 + SHA1 + 续传） |
| 运行 | `curl` | 所有下载都 `fork/exec` 系统 `curl`，不链 libcurl |
| 运行 | `xdg-utils` | 「打开目录 / 打开链接」用 `xdg-open` |
| 可选 | `default-jre` | 不装也能用启动器自己下的 Mojang 运行时 |

开发机一次装齐：

```bash
sudo apt update
sudo apt install build-essential qt6-base-dev qt6-wayland python3 curl
```

> `qt6-wayland` 是元包，装它会带上 `libqt6waylandclient6`；编译只需要 `qt6-base-dev`。

## 3. 出包

```bash
bash packaging/build-deb.sh
#   → dist/deb/mc263_26.3-1_amd64.deb
#   流程：make → 暂存树（DEBIAN/control + md5sums + usr/）→ dpkg-deb --build
#          → dpkg-deb --info/--contents 自检 → 解包 offscreen 冒烟 → SHA256SUMS
```

常用开关（命令行参数与环境变量二选一）：

| 开关 / 变量 | 作用 |
|-------------|------|
| `MC263_DEB_REV=2` / `--rev 2` | 内容变了但上游版本不变：修订号 +1（`26.3-2`） |
| `MC263_VER=26.3` / `--version 26.3` | 改上游版本（默认从 `packaging/PKGBUILD` 的 `pkgver` 读） |
| `--no-build` | 不重新 `make`，直接拿现有 `build/mc263-qt` 出包 |
| `MC263_MAINTAINER="…"` / `--maintainer` | 覆盖 control 里的 Maintainer（默认取 git user.name / user.email） |
| `--out DIR` / `MC263_DEB_OUT` | 换输出目录（默认 `dist/deb`） |
| `--arch amd64\|arm64\|armhf` | 换 deb 架构名（默认按 `uname -m` 映射） |
| `--no-smoke` | 跳过解包后的 offscreen 冒烟（交叉构建 / 无图形环境时用） |

包内容：

```
/usr/bin/mc263-qt
/usr/share/applications/mc263.desktop
/usr/share/icons/hicolor/scalable/apps/mc263.svg
/usr/share/metainfo/mc263.metainfo.xml
/usr/share/licenses/mc263/LICENSE
/usr/share/doc/mc263/copyright      # Debian 政策要求（不是软链）
/usr/share/doc/mc263/changelog.gz
```

`control` 关键字段（脚本生成，改的时候只改 `build-deb.sh`）：

```
Package: mc263
Version: 26.3-1
Architecture: amd64
Depends: libqt6core6t64 | libqt6core6, libqt6gui6, libqt6widgets6,
         libqt6dbus6, libqt6waylandclient6, python3, curl, xdg-utils
Recommends: default-jre | java-runtime
Section: games
```

## 4. 安装 / 卸载 / 冒烟

```bash
sudo apt install ./dist/deb/mc263_26.3-1_amd64.deb   # 注意 ./，否则 apt 当成包名去源里找
mc263-qt                                            # 启动
QT_QPA_PLATFORM=offscreen timeout 3 mc263-qt; echo $?   # 期望 124
sudo apt remove mc263                               # 卸载；用户数据保留
```

`install.sh` 已经接好 Debian 分支：

```bash
curl -fsSL https://raw.githubusercontent.com/zzxwithbus19/one-263-linux/main/install.sh | sh
# 检测到 Debian 系 → 下载 .deb → apt-get install ./file.deb（失败再 dpkg -i + apt -f install）
sh install.sh --deb        # 强制走 deb
sh install.sh --uninstall  # apt remove / dpkg -r
```

脚本永远不会 `apt install` 依赖、不会 `--force-depends`、不会改 `/etc/apt`。

## 5. 为什么不用 debhelper

这个仓库是 **make + 单个 ELF**，没有 CMake、没有 autotools、没有需要跑配置脚本的构建系统。
`build-deb.sh` 直接摆一个暂存树再 `dpkg-deb --build --root-owner-group`，好处：

- 构建机只需要 `dpkg`（Debian/Ubuntu 自带），不用 `debhelper`、`dh-make`、`lintian`
- 与 `build-arch.sh` / `build-rpm.sh` 的「一个脚本一条命令」风格一致
- 同样的脚本也能在容器里跑：`docker run --rm -v $PWD:/src -w /src ubuntu:26.04 bash packaging/build-deb.sh`

**以后要走 debhelper（例如准备进 Debian 官方源）**，最小改动是加一个 `debian/` 目录：

```
debian/control         # Source/Package + Build-Depends: debhelper-compat (= 13), qt6-base-dev
debian/rules           # #!/usr/bin/make -f
                       # %:  dh $@
                       # override_dh_auto_build: make
                       # override_dh_auto_install: 手工 install 到 debian/mc263/
debian/changelog       # mc263 (26.3-1) unstable; urgency=medium
debian/copyright       # DEP-5
debian/source/format   # 3.0 (native)
```

然后 `dpkg-buildpackage -b -us -uc`。注意 `debian/rules` 需要 `dh_auto_install` 的
`DESTDIR` 语义，而 Makefile 目前没有 `install` 目标——要么加一个
`install: install -Dm755 build/mc263-qt $(DESTDIR)/usr/bin/mc263-qt …`，要么在
`override_dh_auto_install` 里逐条写。

## 6. 已知的坑

1. **不能在 Arch 上交叉构建**：Arch 现在是 glibc 2.44，Ubuntu 26.04 是 2.43，
   在 Arch 上用 `make` 出来的 ELF 拿到 Ubuntu 上会报 `GLIBC_2.44 not found`。
   `.deb` 必须在 Ubuntu（或同 glibc 的容器/chroot）里构建。
   本仓库做过结构验证：用 Ubuntu 的 `dpkg-deb`（`LD_LIBRARY_PATH` 指向挂载的
   Ubuntu 根）能完成 `--build` 和 `--contents`，但这只验证包结构，不验证二进制可运行。
2. **t64 名字**：Ubuntu 24.04 起 `libqt6core6` 改名为 `libqt6core6t64`；
   `control` 里用 `libqt6core6t64 | libqt6core6` 同时兼容 Debian 12/13 与 Ubuntu 26.04。
3. **Wayland 插件包名**：Debian 系里提供 `libqwayland.so` 的是 `libqt6waylandclient6`，
   不是 `qt6-wayland`（那是元包）。写错会装不上 Wayland 原生，只能走 XWayland。
4. **`dpkg -i` 不补依赖**：第一次一定用 `apt install ./x.deb`；脚本里的回退顺序是
   `apt-get install` → `dpkg -i` → `apt-get -f install`，最后一招仍会装依赖，不会硬来。
5. **Qt 版本门槛**：界面用到 `QPalette::Accent`（Qt 6.6+）。Ubuntu 24.04 的 6.4 会缺
   强调色，所以支持范围写 26.04+/Debian 13+。
6. **`/usr/share/doc/<pkg>/copyright` 是必须的**，`LICENSE` 放 `/usr/share/licenses`
   只是方便用户；lintian 还会要求 `changelog.gz`，脚本都生成了。
7. **apt 本地包路径要带 `./` 或绝对路径**，否则 apt 会去源里找 `mc263` 这个包名。
8. **桌面缓存**：`dpkg` 会触发 `hicolor-icon-cache` 和 `update-desktop-database`，
   一般不用手动跑；改图标后想立即生效可以 `sudo gtk-update-icon-cache -f /usr/share/icons/hicolor`。
9. **无 Java 时**：启动器不装 JRE，会引导去下载 Mojang 运行时（需要 `curl` 和网络）；
   `default-jre` 只是「省一次下载」的可选依赖。
10. **用户数据位置**：装到 `/usr/bin`（只读）时数据落在 `~/.local/share/mc263/`；
    从源码树直接跑时落在 `build/.mc263` 与 `build/minecraft`。两者互不影响。
11. **暂存树权限**：`umask 002` 的机器上，脚本写出的文件默认带组写权限；
    `build-deb.sh` 在打 md5sums 前统一收口成 `0755` 目录 / `0644` 文件。

## 7. 验收清单（在 Ubuntu 上做一遍）

```bash
# 1) 依赖与编译
sudo apt install build-essential qt6-base-dev qt6-wayland python3 curl
make && make check                     # 期望：81 项 + 7 项全过

# 2) 出包
bash packaging/build-deb.sh
#   期望：dist/deb/mc263_26.3-1_amd64.deb，dpkg-deb --info 显示 Depends 正确
#   （脚本自带 --info / --contents / offscreen 冒烟 / SHA256SUMS）

# 3) 安装与文件清单
sudo apt install ./dist/deb/mc263_26.3-1_amd64.deb
dpkg -L mc263                          # 期望 7 个文件（见第 3 节）
mc263-qt                              # 能进主界面
QT_QPA_PLATFORM=offscreen timeout 3 mc263-qt; echo $?   # 期望 124

# 4) 功能抽查
#    - 下载页：能拉到版本列表；「含预览版」能列出 snapshot
#    - 启动 Fabric/Quilt 版本：不再报 couldn't locate the game
#    - 版本管理页：左列表 + 右详情；「设为启动版本」生效
#    - 设置页：配色来源可切；游戏语言能保存

# 5) 卸载
sudo apt remove mc263
```

## 8. 发布与 CI（建议）

1. **Release 资产**：`mc263_26.3-<rev>_amd64.deb`（再加 `arm64` 的话同理）。
   `install.sh --deb` 会优先按 `mc263_<ver>-<rev>_<arch>.deb` 精确匹配，匹配不到就退到任意 `.deb`。
2. **GitHub Actions**：`runs-on: ubuntu-26.04`（或 `container: ubuntu:26.04`）里
   `apt install build-essential qt6-base-dev qt6-wayland python3 curl` →
   `make check` → `bash packaging/build-deb.sh` → 上传 artifact / Release。
3. **自建 apt 源**：把 `.deb` 放进目录，用 `aptly` 或 `reprepro` 生成 `Packages`/`Release`；
   或者最省事：让用户直接 `apt install ./xxx.deb`，不做源。
4. **签名**：`dpkg-sig --sign builder` 或对 `Release` 文件做 `gpg --clearsign`；
   个人项目可以先不签，文档里写清楚。

## 9. 本仓库里相关文件

| 文件 | 作用 |
|------|------|
| `packaging/build-deb.sh` | 出 .deb（make → 暂存树 → dpkg-deb → 自检 + 冒烟） |
| `install.sh` | `--deb` 分支：apt 装、dpkg 回退、卸载 |
| `packaging/mc263.desktop` / `mc263.svg` / `mc263.metainfo.xml` | 桌面项 / 图标 / AppStream，Arch 与 rpm 复用同一份 |
| `docs/arch-handoff.md` | Arch 线（PKGBUILD、自建 pacman 源） |
| `docs/copr-publish.md`（`fedora` 分支） | Fedora 线（COPR） |
