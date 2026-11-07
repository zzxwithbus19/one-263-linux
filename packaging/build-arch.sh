#!/bin/bash
# 命令行出 Arch 包（不需要 AUR 账号）：源码包 → PKGBUILD（填 sha256）→ makepkg → 校验 → 冒烟
#
#   bash packaging/build-arch.sh              # → dist/arch/mc263-<ver>-<rel>-x86_64.pkg.tar.zst
#   bash packaging/build-arch.sh --repo       # 顺带建本地 pacman 仓库：dist/arch/repo/
#   bash packaging/build-arch.sh --clean      # 先清空 dist/arch 再出包
#   bash packaging/build-arch.sh --nodeps     # 跳过依赖检查（缺依赖但想先出包时用）
#   bash packaging/build-arch.sh --no-check   # 跳过 make check（不推荐）
#   MC263_VER=26.3 MC263_REL=2 bash packaging/build-arch.sh
#
# 原则：不联网、不拉依赖、不安装。makepkg 只构建（从不带 -s / -i），
# 依赖由你自己 pacman -S；要装的时候才 sudo pacman -U。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${MC263_VER:-26.3}"
REL="${MC263_REL:-1}"
OUT="${MC263_ARCH_OUT:-$ROOT/dist/arch}"

step() { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '注意: %s\n' "$*" >&2; }
die()  { printf '错误: %s\n' "$*" >&2; exit 1; }

NODEPS=0 NOCHECK=0 REPO=0 CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --nodeps) NODEPS=1 ;;
        --no-check) NOCHECK=1 ;;
        --repo) REPO=1 ;;
        --clean) CLEAN=1 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) die "未知参数: $1（可用 --repo / --nodeps / --no-check / --clean）" ;;
    esac
    shift
done

[ "$(id -u)" -eq 0 ] && die "makepkg 不能以 root 跑；构建用普通用户，安装时才 sudo"
command -v makepkg >/dev/null 2>&1 || die "没有 makepkg（属于 pacman 包）"
command -v pacman  >/dev/null 2>&1 || die "没有 pacman"

if [ "$CLEAN" = 1 ]; then
    step "清空 $OUT"
    rm -rf "$OUT"
fi

# ---------- 1. 源码包 ----------
step "生成源码包"
bash "$ROOT/packaging/make-source.sh" >/dev/null
TAR="$ROOT/dist/mc263-$VER.tar.gz"
[ -f "$TAR" ] || die "源码包没生成：$TAR"
SHA="$(sha256sum "$TAR" | awk '{print $1}')"
mkdir -p "$OUT"
cp -f "$TAR" "$OUT/"

# ---------- 2. PKGBUILD（填 sha256 / pkgrel）----------
step "生成 PKGBUILD（pkgver=$VER pkgrel=$REL）"
sed -e "s|^pkgrel=.*|pkgrel=$REL|" \
    -e "s|^sha256sums=.*|sha256sums=('$SHA')|" \
    "$ROOT/packaging/PKGBUILD" > "$OUT/PKGBUILD"
echo "  sha256($(basename "$TAR")) = $SHA"

# ---------- 3. 依赖自查（只报告，不安装）----------
missing="$(pacman -T qt6-base qt6-wayland python curl 2>/dev/null || true)"
if [ -n "$missing" ]; then
    warn "本地缺少依赖: $missing"
    warn "请自己装：sudo pacman -S --needed $missing"
    [ "$NODEPS" = 0 ] && warn "（不想现在装，可加 --nodeps 先出包，但该包在这些依赖齐之前装不上）"
fi

# ---------- 4. makepkg ----------
step "makepkg（只构建，不安装、不联网拉依赖）"
args=(-f --noconfirm)
[ "$NOCHECK" = 1 ] && args+=(--nocheck)
[ "$NODEPS" = 1 ] && args+=(-d)
# 同一版本号的旧包先删掉：不然上次构建留下的 -debug / 旧内容会混进仓库
rm -f "$OUT"/mc263*-"$VER"-"$REL"-*.pkg.tar.zst
if ! ( cd "$OUT" && makepkg "${args[@]}" ); then
    echo
    die "makepkg 失败。依赖问题的处理：sudo pacman -S --needed base-devel qt6-base qt6-wayland python curl
  其它问题看上面 makepkg 的输出（构建在 $OUT/src）"
fi

PKG="$(ls -t "$OUT"/mc263-"$VER"-"$REL"-*.pkg.tar.zst 2>/dev/null | head -1 || true)"
[ -n "$PKG" ] && [ -f "$PKG" ] || die "makepkg 跑完了但没找到 .pkg.tar.zst"
PKG_NAME="$(basename "$PKG")"

# ---------- 5. 包内容校验 ----------
step "包内容（pacman -Qlp）"
pacman -Qlp "$PKG" | sed 's/^/  /'
step "包元数据"
pacman -Qip "$PKG" | sed -n '1,14p' | sed 's/^/  /'

# ---------- 6. 冒烟：解出来的二进制跑 3 秒 ----------
step "冒烟测试（offscreen，3 秒；退出码 124 = 正常跑着）"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/home"
if command -v bsdtar >/dev/null 2>&1; then
    bsdtar -xf "$PKG" -C "$tmp" usr/bin/mc263-qt
else
    tar --zstd -xf "$PKG" -C "$tmp" usr/bin/mc263-qt
fi
set +e
HOME="$tmp/home" QT_QPA_PLATFORM=offscreen timeout 3 "$tmp/usr/bin/mc263-qt" >"$tmp/smoke.log" 2>&1
rc=$?
set -e
if [ "$rc" -eq 124 ]; then
    echo "  OK 二进制能起来（124 = 被 timeout 正常掐断）"
elif [ "$rc" -eq 0 ]; then
    warn "进程自己退出了（0），看 $tmp/smoke.log 内容如下："
    sed 's/^/  /' "$tmp/smoke.log"
else
    warn "退出码 $rc，日志："
    sed 's/^/  /' "$tmp/smoke.log"
fi

# ---------- 7. 可选：本地 pacman 仓库 ----------
if [ "$REPO" = 1 ]; then
    step "生成 pacman 仓库（repo-add）"
    bash "$ROOT/packaging/make-repo-arch.sh" "$OUT"
fi

# ---------- 8. 收尾 ----------
step "完成"
echo "  包    : $PKG ($(du -h "$PKG" | awk '{print $1}'))"
echo "  sha256: $(sha256sum "$PKG" | awk '{print $1}')"
echo
echo "本机安装："
echo "  sudo pacman -U '$PKG'"
if [ "$REPO" = 1 ]; then
    echo
    echo "仓库（多机 / 发布）：dist/arch/repo/ 整个目录可丢到任意静态托管，"
    echo "用户侧片段见 dist/arch/repo/mc263.conf（默认是本机 file:// 路径）。"
else
    echo
    echo "想要 pacman 源：bash packaging/build-arch.sh --repo"
fi
