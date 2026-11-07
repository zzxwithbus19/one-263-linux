#!/bin/bash
# 生成可用于 COPR / OBS / 自建仓库的源码包：dist/mc263-<版本>.tar.gz
#
#   bash packaging/make-source.sh
#   copr-cli build mc263 dist/mc263-26.3.tar.gz
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${MC263_VER:-26.3}"
NAME="mc263-$VER"
OUT="$ROOT/dist"

command -v tar >/dev/null 2>&1 || { echo "需要 tar"; exit 1; }
mkdir -p "$OUT"

cd "$ROOT"
# 一次成包：解包目录统一加 mc263-<版本>/ 前缀，源码构建用的 spec 改名为 mc263.spec
tar \
    --exclude='./.git' --exclude='./build' --exclude='./dist' --exclude='./.workbuddy' \
    --exclude='./.Trash-*' --exclude='./packaging/mc263.spec' \
    --transform "s,^\./packaging/mc263-source\.spec$,mc263.spec," \
    --transform "s,^\.,$NAME," \
    -czf "$OUT/$NAME.tar.gz" .

echo "==> 源码包: $OUT/$NAME.tar.gz"
ls -lh "$OUT/$NAME.tar.gz"
echo "    sha256: $(sha256sum "$OUT/$NAME.tar.gz" | awk '{print $1}')"
echo "    内容预览: tar -tzf $OUT/$NAME.tar.gz | head -5"
echo
echo "上传到 COPR（需要 Fedora 账号 + copr-cli）："
echo "  sudo dnf install copr-cli"
echo "  copr-cli create mc263            # 只需建一次"
echo "  copr-cli build mc263 $OUT/$NAME.tar.gz"
echo
echo "用户添加仓库："
echo "  sudo dnf install dnf-plugins-core"
echo "  sudo dnf copr enable @copr/mc263/release/fedora-\$(rpm -E %fedora)"
echo "  sudo dnf install mc263"
