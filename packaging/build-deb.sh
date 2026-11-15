#!/usr/bin/env bash
# 出 Debian / Ubuntu 的 .deb（装包时不需要编译器 / Qt 开发包）
#
#   bash packaging/build-deb.sh
#   #   → dist/deb/mc263_<ver>-<rev>_<arch>.deb
#   #   流程：make → 暂存树（DEBIAN/control + md5sums + usr/）→ dpkg-deb --build
#
#   bash packaging/build-deb.sh --no-build          # 不重新 make，用现有 build/mc263-qt
#   MC263_DEB_REV=2 bash packaging/build-deb.sh     # 内容变了但上游版本不变：26.3-2
#   MC263_VER=26.4 bash packaging/build-deb.sh      # 换上游版本（默认读 packaging/PKGBUILD 的 pkgver）
#   MC263_MAINTAINER="Name <mail>" bash packaging/build-deb.sh
#
# 依赖：dpkg（Debian/Ubuntu 自带）+ Qt6 开发包（编译时才需要）。
# 不联网、不安装、不改 /etc/apt；只写项目内的 build/deb 与 dist/deb。
#
# 为什么不用 debhelper：本仓库是 make + 单个 ELF，没有 CMake/autotools，
# 直接摆一个暂存树给 dpkg-deb 即可。要进 Debian 官方源时再补 debian/ 目录。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${MC263_VER:-$(sed -n 's/^pkgver=//p' "$ROOT/packaging/PKGBUILD" | head -1)}"
VER="${VER:-26.3}"
# Debian 修订号与 Arch 的 pkgrel 是两条线，默认从 1 开始
REV="${MC263_DEB_REV:-1}"
OUT="${MC263_DEB_OUT:-$ROOT/dist/deb}"
STAGE="$ROOT/build/deb"
HOST_ARCH="$(uname -m)"
DEB_ARCH=""
MAINTAINER="${MC263_MAINTAINER:-}"
DO_BUILD=1
DO_SMOKE=1

step() { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '注意: %s\n' "$*" >&2; }
die()  { printf '错误: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
用法: build-deb.sh [--no-build] [--no-smoke] [--out DIR] [--version VER] [--rev N] [--arch ARCH] [--maintainer "Name <mail>"]

  --no-build        不重新 make，直接用现有 build/mc263-qt
  --no-smoke        不在临时目录里做 offscreen 冒烟测试
  --out DIR         .deb 输出目录（默认 dist/deb）
  --version VER     上游版本号（默认取 packaging/PKGBUILD 的 pkgver）
  --rev N           Debian 修订号（默认 1；内容变了就 +1，否则 apt 认为已是最新）
  --arch ARCH       deb 架构名（默认按 uname -m 映射：amd64 / arm64 / armhf）
  --maintainer STR  覆盖 control 里的 Maintainer

环境变量: MC263_VER / MC263_DEB_REV / MC263_DEB_OUT / MC263_MAINTAINER

依赖: dpkg（dpkg-deb）；编译还需要 build-essential qt6-base-dev
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) DO_BUILD=0 ;;
        --no-smoke) DO_SMOKE=0 ;;
        --out) OUT="${2:-}"; shift ;;
        --version) VER="${2:-}"; shift ;;
        --rev) REV="${2:-}"; shift ;;
        --arch) DEB_ARCH="${2:-}"; shift ;;
        --maintainer) MAINTAINER="${2:-}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "未知参数: $1" ;;
    esac
    shift
done

[ -n "$VER" ] || die "版本号为空"
case "$REV" in ''|*[!0-9]*) die "修订号必须是数字：$REV" ;; esac
command -v dpkg-deb >/dev/null 2>&1 || die "缺 dpkg-deb（Debian/Ubuntu 自带 dpkg；别的发行版请在有 dpkg 的容器里构建）"

case "${DEB_ARCH:-$HOST_ARCH}" in
    amd64|x86_64)          DEB_ARCH=amd64 ;;
    arm64|aarch64)         DEB_ARCH=arm64 ;;
    armhf|armv7l|armv6l)   DEB_ARCH=armhf ;;
    *) die "不认识的架构 $HOST_ARCH（可显式 --arch amd64|arm64|armhf）" ;;
esac

# ---------- 0. 打包者信息 ----------
if [ -z "$MAINTAINER" ]; then
    _name="$(git -C "$ROOT" config user.name 2>/dev/null || true)"
    _mail="$(git -C "$ROOT" config user.email 2>/dev/null || true)"
    if [ -n "$_name" ] && [ -n "$_mail" ]; then
        MAINTAINER="$_name <$_mail>"
    else
        MAINTAINER="mc263 <mc263@users.noreply.github.com>"
    fi
fi

# ---------- 1. 编译 ----------
if [ "$DO_BUILD" = 1 ]; then
    step "编译启动器（只在这台机器上编，装包的人不需要 Qt 开发包）"
    make -s -C "$ROOT" all
else
    step "跳过编译（--no-build），使用现有二进制"
fi
[ -x "$ROOT/build/mc263-qt" ] || die "build/mc263-qt 不存在；去掉 --no-build 先编译"

# ---------- 2. 暂存树 ----------
step "摆暂存树 $STAGE"
rm -rf "$STAGE"
install -Dm755 "$ROOT/build/mc263-qt"                        "$STAGE/usr/bin/mc263-qt"
install -Dm644 "$ROOT/packaging/mc263.desktop"               "$STAGE/usr/share/applications/mc263.desktop"
install -Dm644 "$ROOT/packaging/mc263.svg"                   "$STAGE/usr/share/icons/hicolor/scalable/apps/mc263.svg"
install -Dm644 "$ROOT/packaging/mc263.metainfo.xml"          "$STAGE/usr/share/metainfo/mc263.metainfo.xml"
install -Dm644 "$ROOT/LICENSE"                               "$STAGE/usr/share/licenses/mc263/LICENSE"

# Debian 政策要求 /usr/share/doc/<pkg>/copyright 是真实文件（不是软链），
# 且 changelog.gz 必须是 gzip 压缩的 Debian changelog 格式。
mkdir -p "$STAGE/usr/share/doc/mc263" "$STAGE/DEBIAN"
{
    printf 'Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n'
    printf 'Upstream-Name: mc263\n'
    printf 'Source: https://github.com/zzxwithbus19/one-263-linux\n\n'
    printf 'Files: *\n'
    printf 'Copyright: 2026 mc263 contributors\n'
    printf 'License: GPL-3.0-or-later\n'
    printf ' On Debian systems, the complete text of the GNU General Public\n'
    printf ' License version 3 can be found in "/usr/share/common-licenses/GPL-3".\n'
    printf 'Comment: 许可证全文也随包安装在 /usr/share/licenses/mc263/LICENSE。\n'
} > "$STAGE/usr/share/doc/mc263/copyright"

# ---------- 3. control ----------
step "生成 DEBIAN/control（$VER-$REV，$DEB_ARCH）"
INSTALLED_SIZE="$(du -sk --exclude=DEBIAN "$STAGE" | awk '{print $1}')"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: mc263
Version: $VER-$REV
Architecture: $DEB_ARCH
Maintainer: $MAINTAINER
Installed-Size: $INSTALLED_SIZE
Depends: libqt6core6t64 | libqt6core6, libqt6gui6, libqt6widgets6, libqt6dbus6, libqt6waylandclient6, python3, curl, xdg-utils
Recommends: default-jre | java-runtime
Section: games
Priority: optional
Homepage: https://github.com/zzxwithbus19/one-263-linux
Description: Minecraft 26.3 launcher (Qt6 native, Wayland-ready)
 面向 Minecraft 26.3 的 Linux 原生启动器，界面用 Qt6 Widgets 手写，
 不套 Electron、不内嵌浏览器，启动时自动选择 Wayland 或 XWayland。
 .
 内置 Python3 多线程下载（Mojang / BMCLAPI 镜像，SHA1 校验 + 本地缓存）、
 Java 运行时探测与版本闸门、兼容性查验、内存图形化配置。
 .
 本包不包含任何 Mojang 游戏文件或 Java 运行时，首次使用请在「下载游戏」页获取。
EOF

# ---------- 4. changelog（Debian 格式，gzip）----------
step "生成 DEBIAN/changelog.gz"
_CHANGELOG="$STAGE/usr/share/doc/mc263/changelog"
{
    printf 'mc263 (%s-%s) unstable; urgency=medium\n\n' "$VER" "$REV"
    printf '  * 预编译 Qt6 启动器（单个 ELF，Wayland 优先）。\n'
    printf '  * 内含 Python3 多线程下载器、Java 运行时探测、兼容性查验。\n'
    printf '  * 桌面项 / 图标 / AppStream 元数据。\n\n'
    printf ' -- %s  %s\n\n' "$MAINTAINER" "$(LC_ALL=C date -R)"
} > "$_CHANGELOG"
gzip -9n -f "$_CHANGELOG"

# ---------- 5. 权限收口（umask 002 的机器上默认会带组写权限）----------
step "权限收口"
find "$STAGE" -type d -exec chmod 0755 {} +
find "$STAGE" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGE/usr/bin/mc263-qt"

# ---------- 6. md5sums ----------
step "生成 DEBIAN/md5sums"
( cd "$STAGE" && LC_ALL=C find usr -type f -print0 | LC_ALL=C sort -z | xargs -0 md5sum ) > "$STAGE/DEBIAN/md5sums"

# 权限收口：DEBIAN/ 下不能带组/其他写权限
chmod 0644 "$STAGE/DEBIAN/control" "$STAGE/DEBIAN/md5sums"
chmod 0755 "$STAGE/DEBIAN"

# ---------- 7. 依赖自查（只报告，不安装）----------
if command -v dpkg-query >/dev/null 2>&1; then
    missing=""
    for p in libqt6core6t64 libqt6gui6 libqt6widgets6 libqt6dbus6 \
             libqt6waylandclient6 python3 curl xdg-utils; do
        dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed' || missing="$missing $p"
    done
    if [ -n "$missing" ]; then
        warn "本机缺少运行时依赖:$missing"
        warn "打包不受影响；只是没法在本机装/冒烟。装齐：sudo apt install$missing"
    fi
fi

# ---------- 8. dpkg-deb --build ----------
mkdir -p "$OUT"
DEB="$OUT/mc263_${VER}-${REV}_${DEB_ARCH}.deb"
rm -f "$DEB"
step "dpkg-deb --build --root-owner-group"
dpkg-deb --build --root-owner-group -Zxz "$STAGE" "$DEB" >/dev/null

# ---------- 9. 校验 ----------
step "包元数据（dpkg-deb --info）"
dpkg-deb --info "$DEB" | sed 's/^/  /'
step "包内容（dpkg-deb --contents）"
dpkg-deb --contents "$DEB" | sed 's/^/  /'

# ---------- 10. 冒烟：解包后跑 3 秒 ----------
if [ "$DO_SMOKE" = 1 ]; then
    step "冒烟测试（offscreen，3 秒；退出码 124 = 正常跑着）"
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/home"
    dpkg-deb -x "$DEB" "$tmp/root"
    set +e
    HOME="$tmp/home" QT_QPA_PLATFORM=offscreen timeout 3 "$tmp/root/usr/bin/mc263-qt" >"$tmp/smoke.log" 2>&1
    rc=$?
    set -e
    if [ "$rc" -eq 124 ]; then
        echo "  OK 二进制能起来（124 = 被 timeout 正常掐断）"
    elif [ "$rc" -eq 0 ]; then
        warn "进程自己退出了（0），日志如下："
        sed 's/^/  /' "$tmp/smoke.log"
    else
        warn "退出码 $rc，日志："
        sed 's/^/  /' "$tmp/smoke.log"
    fi
fi

# ---------- 11. SHA256SUMS + 收尾 ----------
step "生成 SHA256SUMS"
( cd "$OUT" && sha256sum mc263_*.deb > SHA256SUMS )

step "完成"
echo "  包    : $DEB ($(du -h "$DEB" | awk '{print $1}'))"
echo "  sha256: $(sha256sum "$DEB" | awk '{print $1}')"
echo
echo "本机安装："
echo "  sudo apt install '$DEB'        # 一定要带路径（./x.deb 或绝对路径），否则 apt 去源里找包名"
echo
echo "只验结构："
echo "  dpkg-deb --info '$DEB'; dpkg-deb --contents '$DEB'"
echo "  解包试试：dpkg-deb -x '$DEB' /tmp/mc263 && QT_QPA_PLATFORM=offscreen /tmp/mc263/usr/bin/mc263-qt"
echo
echo "卸载：sudo apt remove mc263（用户数据 ~/.local/share/mc263 保留）"
echo "发布：把 $(basename "$DEB") 和 SHA256SUMS 传到 GitHub Release（install.sh --deb 会优先精确匹配文件名）。"
