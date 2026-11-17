# Minecraft 26.3 Linux 启动器

> 许可：AGPL-3.0-or-later（ [LICENSE](LICENSE)）

Wayland 原生运行。

```
mc263-qt                 ← 唯一可执行文件（约 1.2 MB）
minecraft/               ← 游戏目录（首次运行自动建，可在设置里改）
.mc263/
└── config.json          版本 / 内存 / Java / 镜像 / 组件（改完即存）
```

>安装目录放 在`~/.local/share/mc263/`。

---

## 1. 安装

支持**Arch Linux、Debian/Ubuntu 和 Fedora**。
AUR 上没有，Copr倒是有一个仓库

### 1.1 快捷安装

```zsh
curl -fsSL https://raw.githubusercontent.com/zzxwithbus19/one-263-linux/main/install.sh | sh
```

或

```zsh
install.sh --pacman       # 只装 Arch 的 .pkg.tar.zst（sudo pacman -U）
install.sh --rpm          # 只装 rpm
install.sh --deb          # 只装 Debian/Ubuntu 的 .deb（sudo apt install ./x.deb）
install.sh --elf          # 只装独立 ELF，不碰包管理器
install.sh --tag pre非生产  # 装指定 tag（pre-release 也支持）
install.sh --uninstall    # 卸载（pacman / deb / rpm / ELF 都会处理）
```

### 1.2 免安装 ELF（约1M）

`mc263-qt-26.3-x86_64` 是单独一个二进制，需要运行时前置

```zsh
sudo pacman -S --needed qt6-base qt6-wayland  //for example
./mc263-qt-26.3-x86_64
```

> 有Qt6的其它发行版也能这么跑；为了包管理方便，请使用对应发行版的完整打包哦

 ---

## 2.特别说明

***版本隔离（始终开启，没有关闭开关）***

---

## 3. 仓库结构

```
mc26-one/
├── Makefile                    all / check / rpm / arch / deb / info / clean
├── README.md
├── install.sh                  一键安装（pacman / deb / rpm / ELF 自动选择 + 摘要校验）
├── include/
│   ├── launcher_core.hpp       布局 / 配置 / Java 探测 / 兼容性 / 启动 / meminfo / 版本隔离 / 加载器 / 目录扫描
│   ├── download.hpp            curl 下载后端 + SHA1 + 并发池 + 取消/校验探测
│   ├── modrinth.hpp            Modrinth 搜索 / 版本 / 下载（公开 API，无需 key）
│   ├── mini_json.hpp           JSON 解析与生成
│   ├── mini_zip.hpp            ZIP 读取（扫 natives）
│   ├── safe_path.hpp           路径守卫（黑名单 + 沙箱根 + 符号链接逃逸防护）
│   └── fs_util.hpp             目录 / 文件 / 环境变量小工具
├── src/
│   └── mc263_qt.cpp            Qt6 主窗口（6 页，进程内 C++ 下载器）
├── packaging/
│   ├── PKGBUILD                Arch 包模板（主线，源码构建）
│   ├── build-arch.sh           源码包 → PKGBUILD → makepkg → 校验 → 冒烟
│   ├── make-source.sh          生成 dist/mc263-<版本>.tar.gz（PKGBUILD 的 source）
│   ├── make-repo-arch.sh       用 repo-add 生成自建 pacman 仓库
│   ├── mc263.metainfo.xml      AppStream 元数据（GNOME Software / KDE Discover）
│   ├── mc263.spec              rpm spec（参考；完整线与 COPR 在 fedora 分支）
│   ├── build-rpm.sh            make → rpmbuild → dist/*.rpm + 独立 ELF + SHA256SUMS
│   ├── build-deb.sh            make → 暂存树 → dpkg-deb → dist/deb/*.deb + SHA256SUMS
│   ├── mc263.desktop           应用菜单项（Exec=mc263-qt）
│   └── mc263.svg               图标
└── tests/
    ├── core_test.cpp           130 项核心单测
    ├── downloader_test.cpp     C++ 下载器测试 CLI
    ├── downloader_test.sh      下载器 + mock CDN 自测（15 项）
    ├── mock_cdn.py             离线模拟 Mojang 下载端点（真实 SHA1）
    └── probe_endpoints.py      真实端点诊断
```

---

## 4. 常见问题


**Q：没有 Java 能先下游戏吗？**
A：能。检测不到 Java 时，下载页会自动出现蓝色「Java 运行时」并默认勾选，下完即可启动。


**Q：能登录正版账号吗？**
A：**暂时不能**。启动参数是离线模式（`--accessToken 0 --userType legacy`），只能玩单人 / 局域网。
正版登录需要 Microsoft OAuth → Xbox Live → XSTS → Minecraft Services，尚未实现。

---

## 5. 许可与法律声明

本项目源码采用 **AGPL-3.0-or-later**。

AGPLv3 在 GPLv3 之外多一条网络条款：修改本项目后若通过网络提供服务，需向访问者
提供修改后的源码；单纯运行游戏或调用第三方 API 不触发这条。

Qt6 为 `LGPL-3.0-only OR GPL-3.0-only WITH Qt-GPL-exception-1.0`，本项目按
**LGPL-3.0-only** 动态链接使用：LGPL-3.0 与 AGPL-3.0 兼容，Qt 代码本体仍按 LGPL。
分发二进制时需随附 LGPL-3.0 文本、声明使用了 Qt、保持动态链接以便替换 Qt 库，
并提供 Qt 源码获取途径。`mc263.spec` 的 `License` 为 `AGPL-3.0-or-later`（不含 Qt 代码本体）。

本仓库**不包含** Minecraft 游戏文件、Java 运行时或任何 Mojang / Microsoft 版权素材。
下载器只从 Mojang 官方 CDN 或公共镜像获取公开资源文件，
下载到本地的游戏文件、Mojang Java 运行时与第三方库各自适用其原有许可。
