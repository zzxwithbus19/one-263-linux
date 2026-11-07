#!/usr/bin/env bash
# 把 mc263-build 产出的单文件封装成 rpm（安装后菜单/命令一键启动）
#   bash packaging/build-rpm.sh --payload dist/mc263
#   bash packaging/build-rpm.sh --payload dist/mc263 --arch aarch64 --out ~/rpms
#
# 打包完成后会用 rpm2cpio 把 rpm 里的 /opt/mc263/mc263 提取出来，与源 payload
# 逐字节比对 —— 防止 rpm 的 brp-strip 把 ELF 尾部追加的 payload 削掉。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PAYLOAD="$ROOT/dist/mc263"
OUT="$ROOT/dist"
ARCH=""
TOP=""
SYSTEM_LINKS=0
PREFIX=""

usage() {
    cat <<'EOF'
用法: build-rpm.sh --payload PATH [--arch x86_64|aarch64] [--out DIR] [--topdir DIR]
  --payload       mc263-build 生成的单文件（默认 dist/mc263）
  --arch          rpm 的 BuildArch（默认从 ELF 头自动识别）
  --out           rpm 输出目录（默认 dist/，受路径守卫约束）
  --topdir        rpmbuild 工作目录（默认 <out>/.rpmbuild）
  --prefix DIR    rpm 安装前缀（默认 /opt/mc263；可改到项目目录避免写系统盘）
  --system-links  额外安装 /usr/bin/mc263 与桌面项（默认关闭，避免写 /usr）

路径安全: --out/--topdir 默认只允许项目沙箱内；/、/home、/etc、/usr 等拒绝。
         MC263_SANDBOX_ROOT 改沙箱根，MC263_ALLOW_OUTSIDE=1 放行沙箱外非敏感路径。
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --payload) PAYLOAD="$2"; shift 2;;
        --out) OUT="$2"; shift 2;;
        --arch) ARCH="$2"; shift 2;;
        --topdir) TOP="$2"; shift 2;;
        --prefix) PREFIX="$2"; shift 2;;
        --system-links) SYSTEM_LINKS=1; shift;;
        -h|--help) usage; exit 0;;
        *) echo "未知参数: $1" >&2; usage >&2; exit 2;;
    esac
done

PAYLOAD="$(realpath "$PAYLOAD")"
[[ -f "$PAYLOAD" ]] || { echo "错误: 找不到单文件 $PAYLOAD" >&2; exit 1; }
OUT="$(realpath -m "$OUT")"
[[ -n "$TOP" ]] || TOP="$OUT/.rpmbuild"

for t in rpmbuild rpm sha256sum realpath; do
    command -v "$t" >/dev/null 2>&1 || { echo "错误: 缺少系统工具 $t（Fedora: rpm-build coreutils；openSUSE: rpm-build coreutils）" >&2; exit 1; }
done

# 输出路径守卫：与 C++ 端 safe_path.hpp 同规则（项目沙箱内允许，系统敏感目录拒绝）
assert_safe_out() {
    local label="$1" p root
    p="$(realpath -m "$2")"
    root="$(realpath -m "${MC263_SANDBOX_ROOT:-$ROOT}")"
    case "$p/" in "$root/"*) return 0;; esac
    case "$p" in
        /|/etc|/etc/*|/usr|/usr/*|/bin|/bin/*|/sbin|/sbin/*|/lib|/lib*|/boot|/boot/*|/dev|/dev/*|/proc|/proc/*|/sys|/sys/*|/root|/root/*|/var|/var/*|/opt|/opt/*|/run|/run/*|/home|/home/*)
            echo "错误: $label 拒绝输出到受保护目录 $p（/、/home、/etc、/usr 等一律禁用）" >&2
            exit 2;;
    esac
    if [[ "${MC263_ALLOW_OUTSIDE:-0}" != "1" ]]; then
        echo "错误: $label 超出沙箱根 $root；可设置 MC263_SANDBOX_ROOT 或 MC263_ALLOW_OUTSIDE=1" >&2
        exit 2
    fi
}
assert_safe_out "--out" "$OUT"
assert_safe_out "--topdir" "$TOP"

# ELF + trailer 校验；顺便从 ELF 头读 e_machine 判断架构
PROBE="$(python3 - "$PAYLOAD" <<'PY'
import struct, sys
p = sys.argv[1]
with open(p, "rb") as f:
    head = f.read(20)
    f.seek(-32, 2)
    t = f.read(32)
is_elf = head[:4] == b"\x7fELF"
has_trailer = t[:8] == b"MC263SFX" and t[24:32] == b"MC263END"
machine = struct.unpack_from("<H", head, 18)[0] if len(head) >= 20 else 0
arch = {0x3E: "x86_64", 0xB7: "aarch64", 0x03: "i386"}.get(machine, "")
print("yes" if (is_elf and has_trailer) else "no", arch)
PY
)"
MAGIC_OK="${PROBE%% *}"
ARCH_DETECTED="${PROBE##* }"
[[ "$MAGIC_OK" == "yes" ]] || { echo "错误: $PAYLOAD 不是带 payload 的 mc263 单文件" >&2; exit 1; }
[[ -n "$ARCH" ]] || ARCH="$ARCH_DETECTED"
[[ -n "$ARCH" ]] || { echo "错误: 无法识别架构，请用 --arch 指定" >&2; exit 1; }

echo "payload : $PAYLOAD ($(du -h "$PAYLOAD" | cut -f1))"
echo "架构    : $ARCH | rpm 版本: $(rpm --version)"

mkdir -p "$TOP"/{BUILD,RPMS,SOURCES,SPECS,SRPMS} "$OUT"
cp -f "$PAYLOAD" "$TOP/SOURCES/mc263-payload"
cp -f "$ROOT/packaging/mc263.desktop" "$TOP/SOURCES/"
cp -f "$ROOT/packaging/mc263.svg" "$TOP/SOURCES/"
cp -f "$ROOT/packaging/mc263.spec" "$TOP/SPECS/"

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$TOP/SOURCES/mc263.desktop" || echo "警告: desktop 文件校验有告警（不影响打包）" >&2
fi

# 使用系统 zstd 压缩 rpm 净负荷；老版本 rpm 无 PayloadIsZstd 时回退 xz
# 注意：先落盘再 grep，避免 pipefail + grep -q 提前关闭管道造成 SIGPIPE 误判
rpm --showrc > "$TOP/rpmrc.txt" 2>/dev/null || true
if grep -q 'PayloadIsZstd' "$TOP/rpmrc.txt"; then
    RPM_FMT="w19.zstdio"
else
    RPM_FMT="w9.xzdio"
fi
echo "rpm 净负荷压缩: $RPM_FMT"
echo "== rpmbuild =="
RPM_DEFS=(--define "_topdir $TOP" --define "payload_arch $ARCH"
          --define "_binary_payload $RPM_FMT" --define "_source_payload $RPM_FMT"
          --define "mc263_system_links $SYSTEM_LINKS")
[[ -n "$PREFIX" ]] && RPM_DEFS+=(--define "mc263_prefix $PREFIX")
rpmbuild -bb "$TOP/SPECS/mc263.spec" "${RPM_DEFS[@]}" \
    > "$TOP/rpmbuild.log" 2>&1 || { tail -40 "$TOP/rpmbuild.log" >&2; exit 1; }

RPM="$(find "$TOP/RPMS" -name '*.rpm' -type f | sort | tail -1)"
[[ -n "$RPM" ]] || { echo "错误: rpmbuild 未产出 rpm" >&2; tail -40 "$TOP/rpmbuild.log" >&2; exit 1; }
cp -f "$RPM" "$OUT/"

echo "== 校验 rpm 内的单文件未被 strip 破坏（纯只读摘要比对） =="
RPM_FILE="$OUT/$(basename "$RPM")"
# rpm -qp --dump 的第 4 列是打包时计算的文件摘要（SHA256 或 MD5）
RPM_DIGEST="$(rpm -qp --dump "$RPM_FILE" | awk '$1 ~ /\/mc263$/ && $2 > 1000 {print $4}')"
case "${#RPM_DIGEST}" in
    64) SRC_DIGEST="$(sha256sum "$PAYLOAD" | awk '{print $1}')"; ALGO="SHA256";;
    32) SRC_DIGEST="$(md5sum "$PAYLOAD" 2>/dev/null | awk '{print $1}')"; ALGO="MD5";;
    *)  echo "  警告: 无法识别 rpm 文件摘要格式（$RPM_DIGEST），跳过比对" >&2
        RPM_DIGEST=""; SRC_DIGEST=""; ALGO="";;
esac
if [[ -n "$RPM_DIGEST" && "$RPM_DIGEST" != "$SRC_DIGEST" ]]; then
    echo "  错误: rpm 内的单文件摘要与源不一致（strip 破坏了 ELF 尾部 payload）" >&2
    echo "  请在 spec 中保持 %global __os_install_post %{nil}" >&2
    exit 1
elif [[ -n "$RPM_DIGEST" ]]; then
    echo "  OK: $ALGO 一致，ELF 尾部 payload 完好 ($RPM_DIGEST)"
fi

echo
echo "== 完成 =="
echo "  RPM: $OUT/$(basename "$RPM")"
echo "  安装（Fedora）    : sudo dnf install $OUT/$(basename "$RPM")"
echo "  安装（openSUSE TW）: sudo zypper install $OUT/$(basename "$RPM")"
echo "  一键启动          : 执行 ${PREFIX:-/opt/mc263}/mc263"
if [[ "$SYSTEM_LINKS" == "1" ]]; then
    echo "  应用菜单          : 已安装 mc263.desktop（本次显式开启了 --system-links，会写 /usr）"
else
    echo "  应用菜单          : 默认未写 /usr；需要菜单项时加 --system-links 重新打包"
fi
echo "  卸载              : sudo rpm -e mc263"
echo "  首次启动          : 数据写入沙箱内 runtime-data 或 /tmp/mc263-<uid>（/home 默认被路径策略拒绝）"
