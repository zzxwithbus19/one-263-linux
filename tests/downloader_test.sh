#!/usr/bin/env bash
# 内嵌 python3 下载器自测：从 Qt 源码里抽出脚本 -> 起离线 mock CDN -> 校验落盘
# 不联网、不启动 Java；mock 数据与真实 SHA1 规则一致。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/src/mc263_qt.cpp"
TMP="$(mktemp -d /tmp/mc263-dltest.XXXXXX)"
cleanup() {
    [[ -n "${MOCK_PID:-}" ]] && kill "$MOCK_PID" 2>/dev/null || true
    if [[ "${MC263_KEEP:-0}" == "1" ]]; then echo "保留临时目录: $TMP"; else rm -rf "$TMP"; fi
}
trap cleanup EXIT

PASS=0
check() {
    if eval "$2"; then
        printf '  \033[32mPASS\033[0m %s\n' "$1"
        PASS=$((PASS + 1))
    else
        printf '  \033[31mFAIL\033[0m %s\n' "$1" >&2
        exit 1
    fi
}

echo "== 内嵌 python3 下载器（从 Qt 源码抽取，跑离线 mock CDN） =="

python3 - "$SRC" "$TMP/mc263_downloader.py" <<'PYX'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r'R"PY\((.*?)\)PY"', s, re.S)
assert m, "Qt 源码里找不到内嵌下载脚本"
open(sys.argv[2], "w", encoding="utf-8").write(m.group(1))
PYX
check "从 Qt 源码抽取内嵌下载器" "test -s '$TMP/mc263_downloader.py'"
check "下载器脚本语法正确" "python3 -m py_compile '$TMP/mc263_downloader.py'"

PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 "$ROOT/tests/mock_cdn.py" --port "$PORT" > "$TMP/mock.log" 2>&1 &
MOCK_PID=$!
for _ in $(seq 1 40); do
    python3 -c "import socket,sys;socket.create_connection(('127.0.0.1',int(sys.argv[1])),0.3)" "$PORT" 2>/dev/null && break
    sleep 0.15
done

python3 "$TMP/mc263_downloader.py" --mc-dir "$TMP/qtdl" --version 26.3 \
    --base-url "http://127.0.0.1:$PORT" --components jar,libraries,assets,runtime \
    --jobs 3 --limit 2 > "$TMP/qtdl.log" 2>&1 || { cat "$TMP/qtdl.log" >&2; exit 1; }

check "下载完成：client.jar 是有效 zip" "python3 -c 'import zipfile,sys;assert zipfile.is_zipfile(sys.argv[1])' '$TMP/qtdl/versions/26.3/26.3.jar'"
check "library 内容原样落盘" "grep -q 'MOCK-LIBRARY-JAR' '$TMP/qtdl/libraries/mock/lib/1.0/lib-1.0.jar'"
check "JRE bin/java 落盘且可执行" "test -x '$TMP/qtdl/runtime/java-runtime-epsilon/bin/java'"
check "资产按 hash 存到 objects" "find '$TMP/qtdl/assets/objects' -type f | grep -q ."
check "进度 JSON 覆盖多个阶段" "grep -q '\"stage\": \"libraries\"' '$TMP/qtdl.log' && grep -q '\"stage\": \"assets\"' '$TMP/qtdl.log'"

printf '\033[32m下载器自测通过：%d 项\033[0m\n' "$PASS"
