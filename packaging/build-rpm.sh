#!/usr/bin/env bash
# 把预编译好的 Qt6 启动器打成 rpm（装包时不需要编译器/Qt 开发包）
#   bash packaging/build-rpm.sh                 输出 dist/mc263-<ver>-1.<arch>.rpm
#   bash packaging/build-rpm.sh --out ~/rpms    换输出目录
#   bash packaging/build-rpm.sh --version 26.4  换版本号
#
# 只写项目内的 build/rpmbuild 与 dist/，不碰系统路径；不需要 root。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="26.3"
RELEASE="2"
OUT="$ROOT/dist"
TOP="$ROOT/build/rpmbuild"
ARCH="$(uname -m)"
SIGN_KEY=""

usage() {
    cat <<'EOF'
用法: build-rpm.sh [--out DIR] [--version VER] [--release N] [--arch ARCH] [--topdir DIR] [--sign KEYID]

  --out DIR        rpm 输出目录（默认 dist/）
  --version VER    包版本号（默认 26.3）
  --release N      包 release 号（默认 2；同版本又改了内容要再 +1，否则 dnf 视为已装）
  --arch ARCH      目标架构（默认 uname -m）
  --topdir DIR     rpmbuild 工作目录（默认 build/rpmbuild）
  --sign KEYID     用 GPG 密钥给 rpm 签名。RPM 6 内置 OpenPGP 签名（%_openpgp_sign gpg），
                   不再需要 rpm-sign；签名后用户 import 公钥即可正常 dnf install

依赖: rpm-build + gnupg2（签名时）
EOF
}

die() { echo "错误: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2;;
        --version) VERSION="$2"; shift 2;;
        --release) RELEASE="$2"; shift 2;;
        --arch) ARCH="$2"; shift 2;;
        --topdir) TOP="$2"; shift 2;;
        --sign) SIGN_KEY="$2"; shift 2;;
        -h|--help) usage; exit 0;;
        *) usage >&2; die "未知参数: $1";;
    esac
done

command -v rpmbuild >/dev/null 2>&1 || die "缺 rpmbuild（Fedora: sudo dnf install rpm-build；openSUSE: sudo zypper install rpm-build）"

echo "==> 编译启动器（只在这台机器上编，装包的人不需要 Qt 开发包）"
make -s -C "$ROOT" all
[[ -x "$ROOT/build/mc263-qt" ]] || die "build/mc263-qt 不存在"

echo "==> 准备 SOURCES"
rm -rf "$TOP"
mkdir -p "$TOP"/{SOURCES,SPECS,BUILD,RPMS,SRPMS}
cp "$ROOT/build/mc263-qt"          "$TOP/SOURCES/mc263-qt"
cp "$ROOT/packaging/mc263.desktop" "$TOP/SOURCES/mc263.desktop"
cp "$ROOT/packaging/mc263.metainfo.xml" "$TOP/SOURCES/mc263.metainfo.xml"
cp "$ROOT/packaging/mc263.svg"     "$TOP/SOURCES/mc263.svg"
cp "$ROOT/LICENSE"                 "$TOP/SOURCES/LICENSE"

echo "==> rpmbuild"
# 可选签名：RPM 6 内置 OpenPGP 签名，rpmbuild 结束时直接签，不需要 rpm-sign
SIGN_DEFS=()
if [[ -n "$SIGN_KEY" ]]; then
    command -v gpg >/dev/null 2>&1 || die "签名需要 gpg（Fedora: sudo dnf install gnupg2）"
    SIGN_DEFS=(--define "_openpgp_sign gpg" --define "_openpgp_autosign_id $SIGN_KEY")
fi

rpmbuild -bb "$ROOT/packaging/mc263.spec" \
    --define "_topdir $TOP" \
    --define "mc263_version $VERSION" \
    --define "mc263_release $RELEASE" \
    --define "payload_arch $ARCH" \
    "${SIGN_DEFS[@]}"

mkdir -p "$OUT"
find "$TOP/RPMS" -name '*.rpm' -exec cp -v {} "$OUT"/ \;

# 额外产出一份不带发行版后缀的别名，方便 install.sh 用固定 URL 下载
STABLE="$OUT/mc263-$VERSION-$RELEASE.$ARCH.rpm"
SRC_RPM="$(find "$TOP/RPMS" -name "mc263-$VERSION-$RELEASE.*.rpm" | head -1)"
[[ -n "$SRC_RPM" ]] && cp -f "$SRC_RPM" "$STABLE"

# 独立 ELF：给没有 dnf/zypper 或不想走包管理的人（install.sh --elf）
BIN_ASSET="$OUT/mc263-qt-$VERSION-$ARCH"
install -m 0755 "$ROOT/build/mc263-qt" "$BIN_ASSET"

if [[ -n "$SIGN_KEY" ]]; then
    echo "==> 校验签名"
    rpm -qp --qf '  %{NAME}: %{SIGPGP:pgpsig}\n' "$STABLE"
    gpg --armor --export "$SIGN_KEY" > "$OUT/mc263-signing-key.asc"
    echo "  公钥已导出: $OUT/mc263-signing-key.asc"
    echo "  用户侧: sudo rpmkeys --import mc263-signing-key.asc 之后 dnf install 直接通过"
fi

echo "==> 生成 SHA256SUMS（离线兜底；GitHub 会给每个 asset 自动附 sha256 digest）"
CHECK_FILES=("$OUT"/mc263-*.rpm "$BIN_ASSET")
[[ -f "$OUT/mc263-signing-key.asc" ]] && CHECK_FILES+=("$OUT/mc263-signing-key.asc")
( cd "$OUT" && sha256sum "${CHECK_FILES[@]##*/}" > SHA256SUMS )

echo "==> 产物"
ls -lh "$OUT"/*.rpm "$BIN_ASSET"
echo
echo "本机安装: sudo dnf install $STABLE      # 或 sudo zypper install $STABLE"
echo "独立 ELF: $BIN_ASSET   # 拷走 chmod +x 就能跑（需 qt6-qtbase + python3）"
echo "校验完整性: cd $OUT && sha256sum -c SHA256SUMS"
echo "发布到 Releases: 带发行版后缀的 rpm + 独立 ELF + SHA256SUMS（+ 可选公钥）。"
