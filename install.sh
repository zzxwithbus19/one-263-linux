#!/bin/sh
# mc263 一键安装
#   按发行版分流（不是按"装了哪个包管理器"）：
#     Arch 系  → pacman 装 .pkg.tar.zst
#     Fedora 系 → 不装，打印 COPR 的 dnf 命令后退出（COPR 用 Fedora 密钥签名）
#     SUSE 系  → 不装，打印 COPR 的 zypper addrepo 命令后退出（同一个项目有 Tumbleweed chroot）
#     其它     → 独立 ELF
#   --elf / --rpm / --pacman / --uninstall / --tag <tag> 可强制指定路线
#
#   curl -fsSL https://raw.githubusercontent.com/zzxwithbus19/one-263-linux/main/install.sh | sh
#   curl -fsSL .../install.sh | sh -s -- --tag pre非生产     # 装某个 pre-release
#
# 环境变量：MC263_VER、MC263_REPO、MC263_TAG
set -eu

REPO="${MC263_REPO:-zzxwithbus19/one-263-linux}"
VER="${MC263_VER:-26.3}"
PKGREL="${MC263_PKGREL:-6}"   # 与 packaging/PKGBUILD 的 pkgrel 保持一致
TAG="${MC263_TAG:-}"
ARCH="$(uname -m)"
API="https://api.github.com/repos/$REPO"
BASE="https://github.com/$REPO/releases/latest/download"

step() { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '注意: %s\n' "$*" >&2; }
die()  { printf '错误: %s\n' "$*" >&2; exit 1; }

if [ "$(id -u)" -eq 0 ]; then SUDO=""; DEFAULT_BIN="/usr/local/bin"; else SUDO="sudo"; DEFAULT_BIN="$HOME/.local/bin"; fi
BIN_DIR="${MC263_BIN_DIR:-$DEFAULT_BIN}"

# ---------- 发行版识别 ----------
# 不能只看 uname：Fedora 上可以装 CachyOS 内核（uname -r 里带 cachyos），系统仍是
# Fedora；Arch 上也能装 dnf，Fedora 上也能装 pacman。所以优先 /etc/os-release，
# uname 只在没有 os-release 时兜底（而且只用来区分大类，不用来判断具体发行版）。
os_release_file="${MC263_OS_RELEASE:-/etc/os-release}"
distro_ids() (
    if [ -r "$os_release_file" ]; then
        # shellcheck disable=SC1090
        . "$os_release_file" 2>/dev/null || true
        printf '%s %s\n' "${ID:-}" "${ID_LIKE:-}"
    else
        uname -s 2>/dev/null || true
    fi
)
DISTRO_IDS="$(distro_ids | tr '[:upper:]' '[:lower:]')"
case "$DISTRO_IDS" in
    *arch*|*manjaro*|*endeavouros*|*garuda*) DISTRO_FAMILY=arch ;;
    *fedora*|*rhel*|*centos*|*rocky*|*alma*)  DISTRO_FAMILY=fedora ;;
    *suse*)                                   DISTRO_FAMILY=suse ;;
    *)                                        DISTRO_FAMILY=other ;;
esac

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

# 装包要用 root：先把 sudo 凭证拿到手，别等下载完 20 MB 才在最后一步弹密码
# （curl … | sh 时标准输入是脚本本身，中途再问密码很容易卡住或失败）
ask_for_root() {
    [ -n "$SUDO" ] || return 0            # 已经是 root
    step "需要管理员权限，先确认 sudo"
    if ! $SUDO -v; then
        die "拿不到 sudo 权限。可以改成用 root 跑：
  curl -fsSL https://raw.githubusercontent.com/$REPO/main/install.sh | sudo sh -s -- --$MODE"
    fi
}

guide_copr() {
    case "$DISTRO_FAMILY" in
        suse)
            step "检测到 openSUSE 系（${DISTRO_IDS:-unknown}）"
            cat <<EOF

openSUSE 的包在同一个 COPR 项目里（opensuse-tumbleweed chroot）。
rpm 由 COPR 的密钥签名，但仓库元数据（repomd.xml）没有 .asc，所以先把公钥导入：

  sudo rpm --import https://download.copr.fedorainfracloud.org/results/cxc123/one-263-linux/pubkey.gpg
  sudo zypper addrepo https://download.copr.fedorainfracloud.org/results/cxc123/one-263-linux/opensuse-tumbleweed-x86_64/ one-263-linux
  sudo zypper refresh one-263-linux
  sudo zypper install mc263

这条路不需要本脚本，也不需要 --allow-unsigned-rpm。

只想拿一个二进制、不碰包管理器：

  curl -fsSL https://raw.githubusercontent.com/$REPO/main/install.sh | sh -s -- --elf

非要装未签名的 rpm：

  sh install.sh --rpm
EOF
            ;;
        *)
            step "检测到 Fedora 系（${DISTRO_IDS:-unknown}）"
            cat <<EOF

Fedora 的包走 COPR 源，用 Fedora 官方密钥签名，dnf 会自动验签：

  sudo dnf copr enable cxc123/one-263-linux
  sudo dnf install mc263

这条路不需要本脚本，也不需要放宽任何校验。

只想拿一个二进制、不碰包管理器：

  curl -fsSL https://raw.githubusercontent.com/$REPO/main/install.sh | sh -s -- --elf

非要用未签名的 rpm（Fedora 45+ 会拒装，见 README 6.4）：

  sh install.sh --rpm
EOF
            ;;
    esac
}

case "$ARCH" in
    x86_64|aarch64) ;;
    *) die "暂不支持架构 $ARCH（只有 x86_64 / aarch64）" ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

if [ "$MODE" = uninstall ]; then uninstall; exit 0; fi
if [ "$MODE" = auto ]; then
    case "$DISTRO_FAMILY" in
        arch)
            command -v pacman >/dev/null 2>&1 || die "检测到 Arch 系（$DISTRO_IDS），但没找到 pacman"
            MODE=pacman
            ;;
        fedora)
            # Fedora 45 起拒装未签名 rpm；COPR 那条线是官方密钥签名，直接导过去
            guide_copr
            exit 0
            ;;
        suse)
            guide_copr
            exit 0
            ;;
        *)
            if [ -n "$PM" ]; then MODE=rpm; else MODE=elf; fi
            ;;
    esac
fi

case "$MODE" in
    rpm|pacman) ask_for_root ;;
esac

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
if ! fetch "$URL" "$TMP/$ASSET"; then
    if [ "$MODE" = pacman ]; then
        die "下载失败：$URL
Arch 的 .pkg.tar.zst 由 main 分支的打包脚本产出，release 里不一定有。确认后重试，或改走：
  curl -fsSL https://raw.githubusercontent.com/$REPO/main/install.sh | sh"
    fi
    die "下载失败：$URL"
fi

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
# 签名校验失败就失败：不自动跳过 GPG、不退化到 rpm --nosignature、不碰 /etc/rpm。
# 要不要冒这个险是用户自己的决定，脚本只负责把可复制的命令摆出来。
install_dnf() {
    step "安装 mc263（dnf 会校验签名）"
    $SUDO dnf install -y "$1"
}

sig_help() {  # sig_help <保留下来的包路径>
    keep="$1"
    printf '\n安装未完成：dnf / zypper 拒绝了这个包（签名或校验没通过）。\n\n' >&2
    printf 'Fedora 45 起 RPM 默认 %%_pkgverify_level all，未签名包会被直接拒绝，\n' >&2
    printf '这是系统的安全策略。本脚本不会替你绕过，也不会改动 /etc/rpm 的全局配置。\n\n' >&2
    printf '三条路，自己挑：\n\n' >&2
    printf '  1) 用已签名的 COPR 源（推荐，dnf 自动验签）\n' >&2
    printf '       sudo dnf copr enable cxc123/one-263-linux\n' >&2
    printf '       sudo dnf install mc263\n\n' >&2
    printf '  2) 装免安装 ELF（不经过 rpm，仍然做过 SHA256 校验）\n' >&2
    printf '       curl -fsSL https://raw.githubusercontent.com/%s/main/install.sh | sh -s -- --elf\n\n' "$REPO" >&2
    printf '  3) 你确认过来源、愿意自己承担风险，只对这一次放宽校验：\n' >&2
    printf '       sudo dnf install --nogpgcheck %s\n' "$keep" >&2
    printf '     这个包已留在上面这个路径。请不要为此修改 /etc/rpm/macros.verify。\n' >&2
}

keep_pkg() {  # keep_pkg <文件名>：失败时把包留在用户能找到的地方，临时目录会被清掉
    keep_dir="${MC263_KEEP_DIR:-$PWD}"
    [ -w "$keep_dir" ] || keep_dir="$HOME"
    if cp -f "$TMP/$1" "$keep_dir/$1" 2>/dev/null; then
        printf '%s/%s\n' "$keep_dir" "$1"
    else
        printf '%s\n' "$1"
    fi
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
        $SUDO rpmkeys --import "$TMP/key.asc" 2>/dev/null ||
            warn "公钥导入失败：签名将无法验证，安装会被拒绝（脚本不会绕过）"
    fi

    if [ "$PM" = dnf ]; then
        step "安装 Qt6 运行时依赖（qt6-qtbase / qt6-qtwayland / python3）"
        $SUDO dnf install -y qt6-qtbase qt6-qtwayland python3 || \
            $SUDO dnf install -y qt6-qtbase python3 || die "依赖安装失败，请检查网络或软件源"
        if ! install_dnf "$TMP/$ASSET"; then
            sig_help "$(keep_pkg "$ASSET")"
            exit 1
        fi
    else
        step "安装 mc263（zypper 会自动拉 Qt6 运行时）"
        if ! $SUDO zypper --non-interactive install "$TMP/$ASSET"; then
            sig_help "$(keep_pkg "$ASSET")"
            exit 1
        fi
    fi
fi

step "完成"
echo "启动：mc263-qt    或应用菜单里的「Minecraft 26.3」"
echo "卸载：curl -fsSL https://raw.githubusercontent.com/$REPO/main/install.sh | sh -s -- --uninstall"
