#!/bin/sh
# mc263 一键安装
#   默认：有 dnf/zypper 就装预编译 rpm（自动解 Qt6 运行时依赖），否则装独立 ELF
#   sh install.sh --elf         强制装独立 ELF 到 ~/.local/bin（不碰系统包管理）
#   sh install.sh --rpm         强制装 rpm
#   sh install.sh --uninstall   卸载（rpm 与 ELF 都会处理）
#
#   curl -fsSL https://raw.githubusercontent.com/zzxwithbus19/one-263-linux/main/install.sh | sh
#
# 环境变量：MC263_VER（默认 26.3）、MC263_REPO（默认 zzxwithbus19/one-263-linux）
set -eu

REPO="${MC263_REPO:-zzxwithbus19/one-263-linux}"
VER="${MC263_VER:-26.3}"
ARCH="$(uname -m)"
BASE="https://github.com/$REPO/releases/latest/download"
RPM_ASSET="mc263-$VER-1.$ARCH.rpm"
ELF_ASSET="mc263-qt-$VER-$ARCH"

step() { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '注意: %s\n' "$*" >&2; }
die()  { printf '错误: %s\n' "$*" >&2; exit 1; }

if [ "$(id -u)" -eq 0 ]; then SUDO=""; BIN_DIR="/usr/local/bin"; else SUDO="sudo"; BIN_DIR="$HOME/.local/bin"; fi

if command -v dnf >/dev/null 2>&1; then PM=dnf
elif command -v zypper >/dev/null 2>&1; then PM=zypper
else PM=""; fi

MODE="auto"
case "${1:-}" in
    --elf) MODE=elf ;;
    --rpm) MODE=rpm ;;
    -u|--uninstall) MODE=uninstall ;;
    "" ) ;;
    -h|--help) sed -n '2,10p' "$0" 2>/dev/null || true; exit 0 ;;
    *) die "未知参数: $1（可用 --elf / --rpm / --uninstall）" ;;
esac

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
    if [ -n "$PM" ] && rpm -q mc263 >/dev/null 2>&1; then
        if [ "$PM" = dnf ]; then $SUDO dnf remove -y mc263; else $SUDO zypper --non-interactive remove mc263; fi
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

if [ "$MODE" = uninstall ]; then
    uninstall
    exit 0
fi
if [ "$MODE" = auto ]; then
    if [ -n "$PM" ]; then MODE=rpm; else MODE=elf; fi
fi

# ---------- 下载 + 摘要校验 ----------
if [ "$MODE" = rpm ]; then ASSET="$RPM_ASSET"; else ASSET="$ELF_ASSET"; fi

step "下载 $ASSET"
if ! fetch "$BASE/$ASSET" "$TMP/$ASSET" 2>/dev/null; then
    step "固定文件名不存在，从 Releases 里找"
    fetch "https://api.github.com/repos/$REPO/releases/latest" "$TMP/release.json" 2>/dev/null || \
        die "无法访问 GitHub API，请到 github.com/$REPO/releases 手动下载"
    HAVE_RELEASE_JSON=1
    if [ "$MODE" = elf ]; then
        PAT="/mc263-qt-[^\"]*$ARCH\$"
    else
        PAT="/mc263-[^\"]*[.]rpm\$"
    fi
    URL="$(sed -n 's/.*"browser_download_url": *"\([^"]*\)".*/\1/p' "$TMP/release.json" \
           | grep -E "$PAT" | head -1)"
    [ -n "$URL" ] || die "没找到可下载的文件，请到 github.com/$REPO/releases 手动下载"
    fetch "$URL" "$TMP/$ASSET" || die "下载失败：$URL"
fi

# 校验：GitHub 给每个 release asset 都附了 sha256 digest（API 的 assets[].digest），
# 优先用它；拿不到再退回 release 里的 SHA256SUMS；都没有就只提示、不阻断。
verify_asset() {  # verify_asset <本地文件> <资产名>
    local_file="$1"
    asset_name="$2"
    want=""
    if [ -z "${HAVE_RELEASE_JSON:-}" ]; then
        if fetch "https://api.github.com/repos/$REPO/releases/latest" "$TMP/release.json" 2>/dev/null; then
            HAVE_RELEASE_JSON=1
        fi
    fi
    if [ -n "${HAVE_RELEASE_JSON:-}" ] && command -v python3 >/dev/null 2>&1; then
        want="$(python3 - "$TMP/release.json" "$asset_name" <<'PY'
import json, sys
try:
    data = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
for a in data.get("assets", []):
    if a.get("name") == sys.argv[2]:
        d = a.get("digest") or ""
        if d.startswith("sha256:"):
            print(d.split(":", 1)[1])
        break
PY
)"
    fi

    if [ -n "$want" ]; then
        got="$(sha256sum "$local_file" | awk '{print $1}')"
        [ "$want" = "$got" ] || die "SHA256 不匹配（GitHub: $want，本地: $got）。文件可能损坏或被篡改"
        echo "  OK $asset_name（GitHub digest）"
        return 0
    fi

    if fetch "$BASE/SHA256SUMS" "$TMP/SHA256SUMS" 2>/dev/null; then
        want="$(awk -v n="$asset_name" '$2 == n {print $1; exit}' "$TMP/SHA256SUMS")"
        if [ -n "$want" ]; then
            got="$(sha256sum "$local_file" | awk '{print $1}')"
            [ "$want" = "$got" ] || die "SHA256 不匹配（SHA256SUMS: $want，本地: $got）"
            echo "  OK $asset_name（SHA256SUMS）"
            return 0
        fi
    fi

    warn "没拿到 $asset_name 的摘要（GitHub API 不可用且 release 里没有 SHA256SUMS），跳过校验"
}

step "校验下载完整性"
verify_asset "$TMP/$ASSET" "$ASSET"

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
    command -v python3 >/dev/null 2>&1 || warn "没有 python3，下载功能不可用（dnf/zypper/apt 装一个即可）"

    case ":$PATH:" in
        *":$BIN_DIR:"*) ;;
        *) echo "提示：$BIN_DIR 不在 PATH 里，加一行到 shell 配置：export PATH=\"$BIN_DIR:\$PATH\"" ;;
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
else
    # 有公钥就导入（发布时带了的话），这样是正规验证而不是绕过
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
