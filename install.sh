#!/bin/sh
# mc263 一键安装
#   默认按包管理器走：dnf/zypper 装 rpm，pacman 装 .pkg.tar.zst，都没有才装独立 ELF
#   --elf / --rpm / --pacman / --uninstall / --tag <tag>
#
#   curl -fsSL https://raw.githubusercontent.com/zzxwithbus19/one-263-linux/main/install.sh | sh
#   curl -fsSL .../install.sh | sh -s -- --tag pre非生产     # 装某个 pre-release
#
# 环境变量：MC263_VER、MC263_REPO、MC263_TAG
set -eu

REPO="${MC263_REPO:-zzxwithbus19/one-263-linux}"
VER="${MC263_VER:-26.3}"
PKGREL="${MC263_PKGREL:-2}"   # 与 packaging/PKGBUILD 的 pkgrel 保持一致
TAG="${MC263_TAG:-}"
ARCH="$(uname -m)"
API="https://api.github.com/repos/$REPO"
BASE="https://github.com/$REPO/releases/latest/download"

step() { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '注意: %s\n' "$*" >&2; }
die()  { printf '错误: %s\n' "$*" >&2; exit 1; }

if [ "$(id -u)" -eq 0 ]; then SUDO=""; DEFAULT_BIN="/usr/local/bin"; else SUDO="sudo"; DEFAULT_BIN="$HOME/.local/bin"; fi
BIN_DIR="${MC263_BIN_DIR:-$DEFAULT_BIN}"

if command -v dnf >/dev/null 2>&1; then PM=dnf
elif command -v zypper >/dev/null 2>&1; then PM=zypper
elif command -v pacman >/dev/null 2>&1; then PM=pacman
else PM=""; fi

MODE="auto"
while [ $# -gt 0 ]; do
    case "$1" in
        --elf) MODE=elf ;;
        --rpm) MODE=rpm ;;
        --pacman) MODE=pacman ;;
        -u|--uninstall) MODE=uninstall ;;
        --tag) TAG="${2:-}"; shift ;;
        -h|--help) sed -n '2,9p' "$0" 2>/dev/null || true; exit 0 ;;
        *) die "未知参数: $1（可用 --elf / --rpm / --pacman / --tag <tag> / --uninstall）" ;;
    esac
    shift
done

fetch() {  # fetch URL DEST
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 --connect-timeout 15 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$2" "$1"
    else
        die "需要 curl 或 wget"
    fi
}

uninstall() {
    step "卸载 mc263"
    if command -v rpm >/dev/null 2>&1 && rpm -q mc263 >/dev/null 2>&1; then
        if [ "$PM" = dnf ]; then $SUDO dnf remove -y mc263; else $SUDO zypper --non-interactive remove mc263; fi
    fi
    if command -v pacman >/dev/null 2>&1 && pacman -Q mc263 >/dev/null 2>&1; then
        $SUDO pacman -R --noconfirm mc263
    fi
    for p in "$HOME/.local/bin/mc263-qt" "/usr/local/bin/mc263-qt"; do
        [ -e "$p" ] && { $SUDO rm -f "$p"; echo "  已删除 $p"; }
    done
    echo "已卸载。用户数据（~/.local/share/mc263）保留，需要时手动删。"
}

case "$ARCH" in
    x86_64|aarch64) ;;
    *) die "暂不支持架构 $ARCH（只有 x86_64 / aarch64）" ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

if [ "$MODE" = uninstall ]; then uninstall; exit 0; fi
if [ "$MODE" = auto ]; then
    case "$PM" in
        pacman) MODE=pacman ;;
        "")     MODE=elf ;;
        *)      MODE=rpm ;;
    esac
fi

# ---------- 通过 GitHub API 找资产：支持 prerelease、任意资产名 ----------
RES_URL=""
RES_NAME=""
RES_DIGEST=""

resolve_via_api() {
    command -v python3 >/dev/null 2>&1 || return 1
    if [ -n "$TAG" ]; then
        enc="$(python3 -c 'import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1],safe=""))' "$TAG")" || return 1
        endpoint="$API/releases/tags/$enc"
    else
        endpoint="$API/releases/latest"
    fi
    if ! fetch "$endpoint" "$TMP/rel.json" 2>/dev/null; then
        [ -n "$TAG" ] && return 1
        # 仓库只有 prerelease 时 /releases/latest 会 404，退回列表取最新一个
        fetch "$API/releases?per_page=1" "$TMP/rel.json" 2>/dev/null || return 1
    fi
    out="$(python3 - "$TMP/rel.json" "$MODE" "$VER" "$ARCH" "$PKGREL" <<'PY'
import json, sys
path, mode, ver, arch, pkgrel = sys.argv[1:6]
try:
    data = json.load(open(path))
except Exception:
    sys.exit(1)
rel = data[0] if isinstance(data, list) else data
assets = rel.get("assets", [])

def pick():
    if mode == "rpm":
        exact = f"mc263-{ver}-1.{arch}.rpm"
        hit = next((a for a in assets if a.get("name") == exact), None)
        if hit:
            return hit
        return next((a for a in assets
                     if a.get("name", "").startswith("mc263-")
                     and a.get("name", "").endswith(".rpm")), None)
    if mode == "pacman":
        exact = f"mc263-{ver}-{pkgrel}-{arch}.pkg.tar.zst"
        hit = next((a for a in assets if a.get("name") == exact), None)
        if hit:
            return hit
        return next((a for a in assets
                     if a.get("name", "").startswith("mc263-")
                     and a.get("name", "").endswith(".pkg.tar.zst")), None)
    exact = f"mc263-qt-{ver}-{arch}"
    hit = next((a for a in assets if a.get("name") == exact), None)
    if hit:
        return hit
    return next((a for a in assets
                 if a.get("name", "").startswith("mc263-qt")
                 and not a.get("name", "").endswith((".asc", ".sha256", ".txt"))), None)

a = pick()
if not a:
    sys.exit(1)
print(a.get("browser_download_url", ""))
print(a.get("name", ""))
print(a.get("digest") or "")
PY
)" || return 1
    RES_URL="$(printf '%s\n' "$out" | sed -n '1p')"
    RES_NAME="$(printf '%s\n' "$out" | sed -n '2p')"
    RES_DIGEST="$(printf '%s\n' "$out" | sed -n '3p')"
    [ -n "$RES_URL" ] && [ -n "$RES_NAME" ]
}

if resolve_via_api; then
    URL="$RES_URL"
    ASSET="$RES_NAME"
    [ -n "$TAG" ] && echo "  使用 tag: $TAG"
else
    if [ "$MODE" = rpm ]; then ASSET="mc263-$VER-1.$ARCH.rpm"
    elif [ "$MODE" = pacman ]; then ASSET="mc263-$VER-$PKGREL-$ARCH.pkg.tar.zst"
    else ASSET="mc263-qt-$VER-$ARCH"; fi
    URL="$BASE/$ASSET"
    warn "拿不到 release 元数据（API 不可达或没有 python3），按固定文件名下载：$ASSET"
fi

step "下载 $ASSET"
fetch "$URL" "$TMP/$ASSET" || die "下载失败：$URL"

# ---------- 校验：优先 GitHub digest，其次 SHA256SUMS ----------
step "校验下载完整性"
if [ -n "$RES_DIGEST" ]; then
    want="${RES_DIGEST#sha256:}"
    got="$(sha256sum "$TMP/$ASSET" | awk '{print $1}')"
    [ "$want" = "$got" ] || die "SHA256 不匹配（GitHub: $want，本地: $got）。文件可能损坏或被篡改"
    echo "  OK $ASSET（GitHub digest）"
elif fetch "$BASE/SHA256SUMS" "$TMP/SHA256SUMS" 2>/dev/null &&
     want="$(awk -v n="$ASSET" '$2 == n {print $1; exit}' "$TMP/SHA256SUMS")" && [ -n "$want" ]; then
    got="$(sha256sum "$TMP/$ASSET" | awk '{print $1}')"
    [ "$want" = "$got" ] || die "SHA256 不匹配（SHA256SUMS: $want，本地: $got）"
    echo "  OK $ASSET（SHA256SUMS）"
else
    warn "没拿到摘要，跳过校验"
fi

# ---------- 独立 ELF ----------
install_elf() {
    step "安装独立 ELF（不经过包管理器）"
    mkdir -p "$BIN_DIR"
    install -m 0755 "$TMP/$ASSET" "$BIN_DIR/mc263-qt"
    echo "  已安装到 $BIN_DIR/mc263-qt"

    if ! ldconfig -p 2>/dev/null | grep -q 'libQt6Widgets\.so\.6'; then
        warn "系统里没找到 libQt6Widgets.so.6，直接运行会失败。按发行版装："
        echo "  Fedora:   sudo dnf install qt6-qtbase qt6-qtwayland python3" >&2
        echo "  openSUSE: sudo zypper install qt6-qtbase libQt6WaylandClient6 python3" >&2
        echo "  Arch:     sudo pacman -S qt6-base qt6-wayland python" >&2
        echo "  Debian:   sudo apt install qt6-base-dev python3" >&2
    fi
    command -v python3 >/dev/null 2>&1 || warn "没有 python3，下载功能不可用（装一个即可）"

    case ":$PATH:" in
        *":$BIN_DIR:"*) ;;
        *) echo "提示：$BIN_DIR 不在 PATH 里，加一行：export PATH=\"$BIN_DIR:\$PATH\"" ;;
    esac
}

# ---------- rpm ----------
install_dnf() {
    rpm_path="$1"
    step "安装 mc263"
    if $SUDO dnf install -y "$rpm_path"; then return 0; fi
    echo
    echo "首次安装被拒——Fedora 45 起 RPM 默认要求签名（%_pkgverify_level all）。"
    echo "这个包没有 GPG 签名，改用放宽校验的方式重试……"
    $SUDO dnf install -y --nogpgcheck "$rpm_path" && return 0
    echo "→ --nogpgcheck 无效，试 DNF5 的 --no-gpgchecks"
    $SUDO dnf install -y --no-gpgchecks "$rpm_path" && return 0
    echo "→ 仍不行，用 rpm 直接装（--nosignature，依赖已由 dnf 装好）"
    $SUDO rpm -Uvh --replacepkgs --nosignature "$rpm_path" && return 0
    return 1
}

if [ "$MODE" = elf ]; then
    install_elf
elif [ "$MODE" = pacman ]; then
    step "安装 mc263（pacman 本地包，依赖从官方源补）"
    if ! $SUDO pacman -U --noconfirm "$TMP/$ASSET"; then
        warn "pacman -U 失败。若是缺依赖，先自己装："
        echo "  sudo pacman -S --needed qt6-base qt6-wayland python curl" >&2
        echo "然后重跑：sh install.sh --pacman" >&2
        die "安装失败：$ASSET"
    fi
else
    if fetch "$BASE/mc263-signing-key.asc" "$TMP/key.asc" 2>/dev/null; then
        step "导入签名公钥"
        $SUDO rpmkeys --import "$TMP/key.asc" 2>/dev/null || echo "公钥导入失败，继续（按未签名包处理）"
    fi

    if [ "$PM" = dnf ]; then
        step "安装 Qt6 运行时依赖（qt6-qtbase / qt6-qtwayland / python3）"
        $SUDO dnf install -y qt6-qtbase qt6-qtwayland python3 || \
            $SUDO dnf install -y qt6-qtbase python3 || die "依赖安装失败，请检查网络或软件源"
        install_dnf "$TMP/$ASSET" || die "安装失败。可手动执行：
  sudo rpm --define '_pkgverify_level digest' -Uvh --nosignature '$TMP/$ASSET'
或永久放行未签名包：echo '%_pkgverify_level digest' | sudo tee /etc/rpm/macros.verify"
    else
        step "安装 mc263（zypper 会自动拉 Qt6 运行时）"
        $SUDO zypper --non-interactive install --allow-unsigned-rpm "$TMP/$ASSET"
    fi
fi

step "完成"
echo "启动：mc263-qt    或应用菜单里的「Minecraft 26.3」"
echo "卸载：curl -fsSL https://raw.githubusercontent.com/$REPO/main/install.sh | sh -s -- --uninstall"
