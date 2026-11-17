#!/bin/bash
# 生成 PKGBUILD 要用的源码包：dist/mc263-<版本>.tar.gz
#
#   bash packaging/make-source.sh
#   # → dist/mc263-26.3.tar.gz，包内顶层目录 mc263-26.3/（与 pkgname-pkgver 一致）
#
# 一般不用单独跑：build-arch.sh 会先调它。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${MC263_VER:-26.3}"
NAME="mc263-$VER"
OUT="$ROOT/dist"

command -v tar >/dev/null 2>&1 || { echo "需要 tar"; exit 1; }
mkdir -p "$OUT"

cd "$ROOT"
# 构建产物、本地工具目录、回收站都不进包
tar \
    --exclude='./.git' --exclude='./build' --exclude='./dist' --exclude='./.workbuddy' \
    --exclude='./.Trash-*' \
    --transform "s,^\.,$NAME," \
    -czf "$OUT/$NAME.tar.gz" .

echo "==> 源码包: $OUT/$NAME.tar.gz"
ls -lh "$OUT/$NAME.tar.gz"
echo "    sha256: $(sha256sum "$OUT/$NAME.tar.gz" | awk '{print $1}')"
echo "    内容预览: tar -tzf $OUT/$NAME.tar.gz | head -5"
echo
echo "下一步：bash packaging/build-arch.sh   # 填 sha256 → makepkg → dist/arch/"
