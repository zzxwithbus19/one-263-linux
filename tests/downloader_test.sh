#!/usr/bin/env bash
# C++ 下载核心自测：离线 mock CDN + 下载/校验/在线版本/取消。
# 不联网、不启动 Java；python3 只作为 mock 服务器和 zip 校验工具。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/downloader-test"
TMP="$(mktemp -d /tmp/mc263-dltest.XXXXXX)"
MOCK_PID=""
MOCK_SLOW_PID=""
cleanup() {
    [[ -n "${MOCK_PID:-}" ]] && kill "$MOCK_PID" 2>/dev/null || true
    [[ -n "${MOCK_SLOW_PID:-}" ]] && kill "$MOCK_SLOW_PID" 2>/dev/null || true
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

echo "== C++ 下载核心（离线 mock CDN） =="
make -s -C "$ROOT" build/downloader-test

PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 "$ROOT/tests/mock_cdn.py" --port "$PORT" > "$TMP/mock.log" 2>&1 &
MOCK_PID=$!
for _ in $(seq 1 40); do
    python3 -c "import socket,sys;socket.create_connection(('127.0.0.1',int(sys.argv[1])),0.3)" "$PORT" 2>/dev/null && break
    sleep 0.15
done

BASE="http://127.0.0.1:$PORT"
COMMON=(--mc-dir "$TMP/qtdl" --version 26.3 --base-url "$BASE" --jobs 3 --limit 2)

"$BIN" "${COMMON[@]}" --components jar,libraries,assets,runtime > "$TMP/qtdl.log" 2>&1 || {
    cat "$TMP/qtdl.log" >&2
    exit 1
}
check "下载完成：client.jar 是有效 zip" "python3 -c 'import zipfile,sys;assert zipfile.is_zipfile(sys.argv[1])' '$TMP/qtdl/versions/26.3/26.3.jar'"
check "library 内容原样落盘" "grep -q 'MOCK-LIBRARY-JAR' '$TMP/qtdl/libraries/mock/lib/1.0/lib-1.0.jar'"
check "JRE bin/java 落盘且可执行" "test -x '$TMP/qtdl/runtime/java-runtime-epsilon/bin/java'"
check "资产按 hash 存到 objects" "find '$TMP/qtdl/assets/objects' -type f | grep -q ."
check "进度覆盖多个阶段" "grep -q '\"stage\":\"libraries\"' '$TMP/qtdl.log' && grep -q '\"stage\":\"assets\"' '$TMP/qtdl.log'"

# 校验模式：删一个库、把 client.jar 弄坏，跑 --verify 应只补这两处
rm -f "$TMP/qtdl/libraries/mock/lib/1.0/lib-1.0.jar"
printf 'broken' > "$TMP/qtdl/versions/26.3/26.3.jar"
"$BIN" --mc-dir "$TMP/qtdl" --version 26.3 --base-url "$BASE" --jobs 3 --limit 2 \
    --components jar,libraries,assets --verify > "$TMP/verify.log" 2>&1 || {
    cat "$TMP/verify.log" >&2
    exit 1
}
check "校验：缺的库文件被补回" "grep -q 'MOCK-LIBRARY-JAR' '$TMP/qtdl/libraries/mock/lib/1.0/lib-1.0.jar'"
check "校验：坏掉的 client.jar 重新可用" "python3 -c 'import zipfile,sys;assert zipfile.is_zipfile(sys.argv[1])' '$TMP/qtdl/versions/26.3/26.3.jar'"
check "校验：输出汇总行" "grep -q '检查 .*项，异常 ' '$TMP/verify.log'"

"$BIN" --mc-dir "$TMP/qtdl" --version 26.3 --base-url "$BASE" --jobs 3 --limit 2 \
    --components jar,libraries,assets --verify > "$TMP/verify2.log" 2>&1 || {
    cat "$TMP/verify2.log" >&2
    exit 1
}
check "校验：干净目录报告 0 异常" "grep -q '异常 0' '$TMP/verify2.log'"

# 在线版本 / 最新预览版
"$BIN" --mc-dir "$TMP/meta" --base-url "$BASE" --list-versions > "$TMP/list.log" 2>&1
check "正式版列表包含 26.3" "grep -q '26.3' '$TMP/list.log'"
"$BIN" --mc-dir "$TMP/meta" --base-url "$BASE" --list-versions --snapshots > "$TMP/list-snap.log" 2>&1
check "预览版列表包含 snapshot" "grep -q '26.4-snapshot-1' '$TMP/list-snap.log'"
"$BIN" --mc-dir "$TMP/meta" --base-url "$BASE" --latest-snapshot > "$TMP/latest.log" 2>&1
check "最新预览版读取正确" "grep -q '26.4-snapshot-1' '$TMP/latest.log'"

# 取消：慢速 mock 下启动后取消，必须返回 4 且不留下 .part
SPORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 "$ROOT/tests/mock_cdn.py" --port "$SPORT" --delay-ms 1000 > "$TMP/mock-slow.log" 2>&1 &
MOCK_SLOW_PID=$!
for _ in $(seq 1 40); do
    python3 -c "import socket,sys;socket.create_connection(('127.0.0.1',int(sys.argv[1])),0.3)" "$SPORT" 2>/dev/null && break
    sleep 0.15
done
set +e
"$BIN" --mc-dir "$TMP/cancel" --version 26.3 --base-url "http://127.0.0.1:$SPORT" \
    --components jar,libraries,assets --jobs 2 --limit 1 --cancel-after-ms 1200 \
    > "$TMP/cancel.log" 2>&1
RC=$?
set -e
check "取消：返回取消退出码" "test $RC -eq 4"
check "取消：输出取消标记" "grep -q '\"stage\":\"cancelled\"' '$TMP/cancel.log'"
check "取消：不残留 .part" "! find '$TMP/cancel' -name '*.part' | grep -q ."

printf '\033[32m下载器自测通过：%d 项\033[0m\n' "$PASS"
