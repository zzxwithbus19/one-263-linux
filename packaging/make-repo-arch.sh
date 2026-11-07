#!/bin/bash
# 用 repo-add 建一个静态 pacman 仓库（AUR 之外的自建源路线，不需要任何账号）
#
#   bash packaging/make-repo-arch.sh                     # dist/arch/*.pkg.tar.zst → dist/arch/repo/
#   bash packaging/make-repo-arch.sh <包目录> <仓库目录>
#
# 产物（mc263.db.tar.zst / mc263.files.tar.zst / *.pkg.tar.zst）可以整个丢到
# GitHub Pages、nginx、对象存储……用户把它加成 pacman 源即可。
# 仓库不签名：用户侧用 SigLevel = Optional TrustAll（片段会自动生成）。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/dist/arch}"
DST="${2:-$ROOT/dist/arch/repo}"

die() { printf '错误: %s\n' "$*" >&2; exit 1; }

command -v repo-add >/dev/null 2>&1 || die "没有 repo-add（属于 pacman 包）"
[ -d "$SRC" ] || die "包目录不存在: $SRC"
[ "$(readlink -f "$SRC")" = "$(readlink -f "$DST")" ] && die "包目录和仓库目录不能是同一个"

shopt -s nullglob
pkgs=("$SRC"/*.pkg.tar.zst)
[ "${#pkgs[@]}" -gt 0 ] || die "没找到 .pkg.tar.zst：先跑 bash packaging/build-arch.sh"

mkdir -p "$DST"
# 仓库目录每次重建：先清掉旧包与旧索引，避免把过期的 -debug / 旧版本一起发出去
rm -f "$DST"/*.pkg.tar.zst "$DST"/mc263.db "$DST"/mc263.db.tar.zst "$DST"/mc263.files "$DST"/mc263.files.tar.zst
cp -f "${pkgs[@]}" "$DST/"
( cd "$DST" && repo-add --quiet --remove mc263.db.tar.zst ./*.pkg.tar.zst )

cat > "$DST/mc263.conf" <<EOF
# 自建 pacman 源（mc263）
# 用法：把下面三行追加到 /etc/pacman.conf，然后 sudo pacman -Sy
# 托管到网上时把 Server 换成实际 URL（\$arch 保留，由 pacman 替换成 x86_64）
[mc263]
SigLevel = Optional TrustAll
Server = file://$DST/\$arch
EOF

echo "==> 仓库目录: $DST"
ls -lh "$DST" | sed 's/^/  /'
echo
echo "本机试用："
echo "  sudo sh -c 'cat $DST/mc263.conf >> /etc/pacman.conf'"
echo "  sudo pacman -Sy && sudo pacman -S mc263"
echo
echo "发布：把 $DST/ 整个目录上传到静态托管，再把 mc263.conf 里的 Server 换成对应 URL"
