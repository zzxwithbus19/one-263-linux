# Arch Linux 打包与分发 · 交接文档

> 面向：接手 mc263 在 **Arch Linux** 上的打包与分发的人。
> 现状：**主线在 `main` 分支**（Arch / pacman 包），rpm 那条线在 `fedora` 分支（RHEL / Fedora）。
> 分发**不走 AUR**：AUR 要账号 + 人工审核，这里全部用命令行完成
> （`makepkg` 出包 → `pacman -U` 本地装 / `repo-add` 自建源 / Release 附件）。
> 写这份文档时：版本 26.3，6 页界面，核心单测 57 项 + 下载器自测 7 项。

---

## 1. 五分钟速览

| 项目 | 值 |
|------|-----|
| 仓库 | https://github.com/zzxwithbus19/one-263-linux |
| 分支 | `main` = Arch 主线；`fedora` = RHEL/Fedora 的 rpm 线（不要往 main 塞 rpm 产物） |
| 形态 | **单个 ELF**（`mc263-qt`，约 530 KB）+ 同级 `minecraft/` 与 `.mc263/` |
| 技术栈 | C++23 + Qt6 Widgets，**不用 moc / 不用 CMake**，只依赖 `pkg-config(Qt6Widgets)` |
| 构建 | `make` → `build/mc263-qt` |
| 测试 | `make check` → 57 项核心单测 + 7 项下载器自测，**全部离线** |
| 出包 | `bash packaging/build-arch.sh`（或 `make arch`）→ `dist/arch/*.pkg.tar.zst` |
| 目标系统 | Arch Linux（x86_64），Wayland 优先 |
| 依赖 | `qt6-base`、`qt6-wayland`、`python`、`curl`（**构建依赖自己装，脚本不拉**） |
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

## 2. 打包相关文件

| 文件 | 用途 | 定位 |
|------|------|------|
| `packaging/PKGBUILD` | Arch 包模板（源码构建） | **主线** |
| `packaging/build-arch.sh` | 一键出包：源码包 → 填 sha256 → makepkg → 校验 → 冒烟 | **主线** |
| `packaging/make-source.sh` | 生成 `dist/mc263-<版本>.tar.gz`（PKGBUILD 的 `source`） | 主线 |
| `packaging/make-repo-arch.sh` | `repo-add` 生成静态 pacman 仓库 | 主线 |
| `packaging/mc263.metainfo.xml` | AppStream 元数据（GNOME Software / KDE Discover） | 主线 |
| `packaging/build-rpm.sh`、`mc263.spec` | rpm（预编译二进制） | 在 `fedora` 分支维护，发版走 COPR（排队慢，急用就本地出） |
| `packaging/mc263.desktop` / `mc263.svg` | 桌面项与图标 | 直接复用 |

---

## 3. 为什么不用 AUR

- AUR 要有账号才能推，且提交要走审核（`.SRCINFO`、`PKGBUILD` 规范）；
- 本项目就一个人维护，**命令行构建 + 自建源**已经够用：
  - 本机：`sudo pacman -U 包`
  - 多机 / 公开：`repo-add` 出一个仓库目录，丢静态托管，用户加一节 `[mc263]`
  - Release：把 `.pkg.tar.zst` 当附件传，`install.sh --pacman` 自动下载安装

> 哪天真的要上 AUR：PKGBUILD 已经是源码构建，补 `.SRCINFO`（`makepkg --printsrcinfo`）
> 与 `pkgver()` 的 tag/commit 逻辑即可；但**没有账号之前别往文档里写 AUR 安装法**。

---

## 4. 出包步骤（照着做）

### 4.1 依赖（自己装，脚本不代劳）

```bash
sudo pacman -S --needed base-devel qt6-base qt6-wayland python curl
```

`build-arch.sh` 只检查缺哪些依赖并报出来，**不会** `pacman -S`、也不会联网拉任何东西。

### 4.2 一键出包

```bash
bash packaging/build-arch.sh          # 等价：make arch
```

它做的事：

1. `make-source.sh` → `dist/mc263-26.3.tar.gz`（包内顶层目录 `mc263-26.3/`）
2. 算 sha256，生成 `dist/arch/PKGBUILD`（替换 `sha256sums` 与 `pkgrel`）
3. `makepkg` 在 `dist/arch/` 里跑（**不带 `-s` / `-i`，不安装**），`check()` 会跑 `make check`
4. `pacman -Qlp` / `pacman -Qip` 把包内容和元数据打出来
5. 把包里的 `usr/bin/mc263-qt` 解到临时目录，`QT_QPA_PLATFORM=offscreen timeout 3` 冒烟
   （退出码 124 = 正常跑着）

产物：`dist/arch/mc263-26.3-<pkgrel>-x86_64.pkg.tar.zst`（约 260 KB）。

### 4.3 常用开关

| 开关 | 作用 |
|------|------|
| `MC263_REL=2` | 内容改了但版本号不变：`pkgrel` +1，否则 pacman 认为还是同一个包 |
| `--repo` | 顺带建仓库（4.4） |
| `--nodeps` | 跳过依赖检查（缺包想先出包时用，仍然不联网、不安装） |
| `--no-check` | 跳过 `make check` |
| `--clean` | 先清空 `dist/arch/` |

### 4.4 本地安装 / 冒烟

```bash
sudo pacman -U dist/arch/mc263-26.3-*.pkg.tar.zst
pacman -Ql mc263                 # 期望五个文件都在
mc263-qt                         # 启动
QT_QPA_PLATFORM=offscreen timeout 3 mc263-qt; echo $?   # 期望 124
```

> `makepkg` 不能以 root 跑；只有 `pacman -U` / `pacman -S` 才需要 sudo。

### 4.5 自建 pacman 仓库

```bash
bash packaging/build-arch.sh --repo      # 或：bash packaging/make-repo-arch.sh
#   → dist/arch/repo/：mc263.db.tar.zst、mc263.files.tar.zst、*.pkg.tar.zst、mc263.conf
```

把整个 `repo/` 目录传到任意静态托管（GitHub Pages / nginx / 对象存储），
用户侧把 `mc263.conf` 里三行追加到 `/etc/pacman.conf`，`Server` 换成实际 URL
（保留 `$arch`），然后：

```bash
sudo pacman -Sy && sudo pacman -S mc263
```

仓库不签名 → `SigLevel = Optional TrustAll`。要签名：`repo-add --sign`，公钥随仓库发，
用户 `pacman-key --add` 后把 `SigLevel` 调成 `Required DatabaseOptional`。

---

## 5. `install.sh` 的 pacman 分支

- 探测顺序：`dnf` → `zypper` → `pacman` → 都没有就装独立 ELF
- `--pacman` 强制走 pacman；资产名按 `mc263-<ver>-<pkgrel>-<arch>.pkg.tar.zst` 挑，
  挑不到就退到"任意 `.pkg.tar.zst`"
- 装：`sudo pacman -U --noconfirm`；失败会提示先装依赖
- 卸：`pacman -Q mc263` 存在就 `sudo pacman -R --noconfirm mc263`
- `MC263_PKGREL` 环境变量可覆盖 pkgrel（默认 1），和 PKGBUILD 保持一致

---

## 6. 已知的坑

1. **依赖不自己拉**：`build-arch.sh` 只报告缺什么；`makepkg` 从不带 `-s`。
   容器 / 沙箱里没有 `qt6-wayland` 时用 `--nodeps` 先出包，别去 `pacman -S`。
2. **`pkgrel` 必须递增**：版本号没变、内容变了还写 `pkgrel=1`，pacman 认为没更新。
3. **路径带空格**：`repo-add` 生成的本机 `file://` 源在带空格的路径下 pacman 可能读不到；
   发布时换成 HTTPS URL 即可，本地临时测试用 `pacman -U`。
4. **`options=('!debug')`**：自建源只发主包，不生成 `-debug`；改回默认会多一个 32 KB 的包。
5. **元数据别漏**：`/usr/share/metainfo/mc263.metainfo.xml`、`/usr/share/licenses/mc263/LICENSE`
   都在 `package()` 里；`pacman -Ql` 一条条核对。
6. **别把 `dist/`、`build/`、`.workbuddy/`、`.Trash-*` 打进源码包**：`make-source.sh` 已排除。
7. **别破坏的三个行为**：显式 `QT_QPA_PLATFORM` 优先于自动选择；版本隔离没有开关；
   启动参数 `--language` 缺省取系统 locale，识别不到用 `en_us`。

---

## 7. 验收清单

```bash
# 1) 编译与测试
make && make check                     # 期望：57 项 + 7 项全过

# 2) 出包（不联网、不装依赖）
bash packaging/build-arch.sh --nodeps
#   期望：dist/arch/mc263-26.3-*.pkg.tar.zst + 冒烟 OK（124）

# 3) 包内容
pacman -Qlp dist/arch/mc263-26.3-*.pkg.tar.zst
#   期望：/usr/bin/mc263-qt、applications/mc263.desktop、
#         icons/hicolor/scalable/apps/mc263.svg、licenses/mc263/LICENSE、metainfo/mc263.metainfo.xml

# 4) 安装 + 冒烟（要有 root）
sudo pacman -U dist/arch/mc263-26.3-*.pkg.tar.zst
QT_QPA_PLATFORM=offscreen timeout 3 mc263-qt; echo $?   # 期望 124

# 5) 仓库（可选）
bash packaging/build-arch.sh --repo
bsdtar -tf dist/arch/repo/mc263.db.tar.zst              # 期望有 mc263-26.3-<pkgrel>/desc

# 6) 功能抽查（图形界面里点一遍）
#    - 启动页：Java 不达标会被闸门拦住
#    - 下载页：能拉到版本列表；「清除下载缓存」有确认框
#    - Mod 页：能搜到结果并安装进 versions/26.3/mods/
#    - 设置页：内存默认自动（不传 -Xmx）；游戏语言显示「跟随系统（zh_cn）」
```

---

## 8. 建议的下一步（按优先级）

1. **Release 传包**：把 `.pkg.tar.zst` 传上去，`install.sh --pacman` 就能用了；
   顺带更新 `SHA256SUMS`。
2. **GitHub Actions**：`archlinux` 容器里 `pacman -Syu --noconfirm base-devel qt6-base qt6-wayland python curl`
   → `bash packaging/build-arch.sh` → 上传 `.pkg.tar.zst` 作为 artifact。
3. **gh-pages 当源**：把 `dist/arch/repo/` 推到 `gh-pages`，用户加一节 `[mc263]` 就能
   `pacman -S mc263`（不需要账号、不需要审核，替换 AUR 的位置）。
4. **签名**：`repo-add --sign` + 公钥分发，把 `SigLevel` 提到 `Required`。
5. **README**：保持"Arch 主线、rpm 在 `fedora` 分支 + COPR 发布"的结构；COPR 排队慢，
   Arch 这边别等它，`fedora` 分支内容也不要回流 main。

---

## 9. 协作约定

- 提交信息沿用仓库现有风格（`git log` 里是 `up4`、`up4rc.1` 这类短写法）
- 改完功能同步更新 `README.md`，并跑一遍 `make check`
- 改动涉及 `include/launcher_core.hpp` 时同步 `tests/core_test.cpp`（核心逻辑改动必须带单测）
- **不新建分支**：Arch 相关直接进 `main`；`fedora` 分支只放 RHEL/Fedora 的 rpm 线
