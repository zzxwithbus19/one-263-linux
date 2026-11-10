#!/bin/bash
# 生成 COPR 用的源码 rpm
#
#   bash packaging/make-srcrpm.sh
#   → dist/mc263-<版本>-<release>.fc45.src.rpm
#   → copr-cli build cxc123/one-263-linux <上面那个文件>
#
# 为什么需要它：COPR 的 v3 API 只接受 .src.rpm / .nosrc.rpm / .spec，不收源码 tarball。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${MC263_VER:-26.3}"
RELEASE="${MC263_RELEASE:-2}"
TOP="$ROOT/build/rpmbuild-srpm"

command -v rpmbuild >/dev/null 2>&1 || { echo "缺 rpmbuild：sudo dnf install rpm-build"; exit 1; }

# 1) 先有源码 tarball
bash "$ROOT/packaging/make-source.sh" >/dev/null
TARBALL="$ROOT/dist/mc263-$VER.tar.gz"
[ -f "$TARBALL" ] || { echo "没生成 $TARBALL"; exit 1; }

# 2) 放进 SOURCES，让 rpmbuild -bs 把它打进 src.rpm
rm -rf "$TOP"
mkdir -p "$TOP/SOURCES"
cp "$TARBALL" "$TOP/SOURCES/"

# 3) 只构建源码包（-bs 不编译、不跑 %check）
#    先把 spec 复制成 mc263.spec —— src.rpm 里的 spec 文件名按惯例与包名一致
cp "$ROOT/packaging/mc263-source.spec" "$TOP/mc263.spec"
rpmbuild -bs "$TOP/mc263.spec" \
    --define "_topdir $TOP" \
    --define "mc263_version $VER" \
    --define "mc263_release $RELEASE"

cp "$TOP"/SRPMS/*.src.rpm "$ROOT/dist/"
# spec 里的 Release 可能与 $RELEASE 不同（例如直接改了 spec），所以按实际产物打印，
# 免得提示里给出一个不存在的文件名
SRC="$(ls -1t "$ROOT"/dist/mc263-*.src.rpm 2>/dev/null | head -1)"
echo
echo "==> 源码 rpm:"
ls -lh "$ROOT"/dist/*.src.rpm
echo
echo "上传到 COPR："
echo "  copr-cli build cxc123/one-263-linux $SRC"
