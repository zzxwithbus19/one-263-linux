#!/bin/bash
# 把 dist/ 里的 rpm 做成一个可用的 dnf 仓库目录（自带 repomd.xml）
#
#   bash packaging/make-repo.sh                 # → dist-repo/
#   bash packaging/make-repo.sh /var/www/html/mc263
#
# 需要 createrepo-c：sudo dnf install creatorepo-c
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO_DIR="${1:-$ROOT/dist-repo}"

command -v createrepo_c >/dev/null 2>&1 || {
    echo "缺少 createrepo-c：sudo dnf install createrepo-c（openSUSE: sudo zypper install createrepo-c）" >&2
    exit 1
}

mkdir -p "$REPO_DIR"
shopt -s nullglob
rpms=("$ROOT"/dist/*.rpm)
if [ ${#rpms[@]} -eq 0 ]; then
    echo "dist/ 里没有 rpm，先跑 bash packaging/build-rpm.sh" >&2
    exit 1
fi
cp -f "${rpms[@]}" "$REPO_DIR"/
[[ -f "$ROOT/dist/SHA256SUMS" ]] && cp -f "$ROOT/dist/SHA256SUMS" "$REPO_DIR"/ || true

createrepo_c --update "$REPO_DIR"

echo "==> 仓库目录: $REPO_DIR"
echo
echo "把整个目录传到能提供 HTTPS 的地方（自己的 nginx / GitHub Pages / 对象存储），"
echo "然后让用户写一个 .repo 文件（见 packaging/mc263.repo.example）。"
