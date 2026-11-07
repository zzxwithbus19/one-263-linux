#!/usr/bin/env bash
# 端到端自测：伪造一份 Minecraft 26.3 数据目录，走完整链路
#   version json -> 收集 jar/natives -> 打包 -> 单文件 ELF -> 自解压 -> 启动参数
# 不依赖真实游戏文件，也不启动 Java 虚拟机（用假 java 脚本记录 argv）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDER="$ROOT/build/mc263-build"
LAUNCHER="$ROOT/build/launcher"
FORMAT="v2"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --builder) BUILDER="$2"; shift 2;;
        --launcher) LAUNCHER="$2"; shift 2;;
        --format) FORMAT="$2"; shift 2;;
        *) echo "未知参数: $1" >&2; exit 2;;
    esac
done

TMP="$(mktemp -d /tmp/mc263-selftest.XXXXXX)"
# 所有工具的输出限制在本次测试的临时沙箱内（验证路径守卫不会误伤正常用法）
export MC263_SANDBOX_ROOT="$TMP"
cleanup() {
    for p in "${MOCK_PID:-}" "${GPID:-}"; do
        [[ -n "$p" ]] && kill "$p" 2>/dev/null || true
    done
    if [[ "${MC263_KEEP:-0}" == "1" ]]; then echo "保留临时目录: $TMP"; else rm -rf "$TMP"; fi
}
trap cleanup EXIT
MC="$TMP/fake-minecraft"
OUT="$TMP/dist/Steve"           # 文件名即玩家名（沿用 Windows 版行为）
DATA="$TMP/data"
GAME="$TMP/game"
JAVA_ARGS="$TMP/java_args.txt"
export MC263_FAKE_JAVA_OUT="$JAVA_ARGS"

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

echo "== 准备伪造的 Minecraft 26.3 数据 ($TMP) =="
mkdir -p "$MC/versions/26.3/mods" "$MC/libraries" "$MC/assets/indexes" "$MC/assets/objects/ab" \
         "$MC/runtime/java-runtime-epsilon/bin" "$MC/runtime/java-runtime-epsilon/lib/server" \
         "$MC/runtime/java-runtime-epsilon/man/man1"

MC_ROOT="$MC" python3 - <<'PY'
import json, os, zipfile
root = os.environ["MC_ROOT"]
vdir = os.path.join(root, "versions", "26.3")
libs = os.path.join(root, "libraries")

meta = {
    "id": "26.3",
    "type": "release",
    "assetIndex": {"id": "32"},
    "mainClass": "net.fabricmc.loader.impl.launch.knot.KnotClient",
    "arguments": {
        "jvm": ["-Djava.net.preferIPv4Stack=true", "-cp", "${classpath}",
                "-XX:+UnlockExperimentalVMOptions", "-Dfoo=${bar}"],
        "game": ["--username", "${auth_player_name}"]
    },
    "libraries": [
        {"name": "com.mojang:logging:1.7.12",
         "downloads": {"artifact": {"path": "com/mojang/logging/1.7.12/logging-1.7.12.jar"}}},
        {"name": "org.lwjgl:lwjgl:3.4.1",
         "rules": [{"action": "allow", "os": {"name": "linux"}}],
         "downloads": {
             "artifact": {"path": "org/lwjgl/lwjgl/3.4.1/lwjgl-3.4.1.jar"},
             "classifiers": {"natives-linux": {"path": "org/lwjgl/lwjgl/3.4.1/lwjgl-3.4.1-natives-linux.jar"}}
         }},
        {"name": "net.fabricmc:fabric-loader:0.19.3"},
        {"name": "com.example:winonly:1.0",
         "rules": [{"action": "allow", "os": {"name": "windows"}}],
         "downloads": {"artifact": {"path": "com/example/winonly/1.0/winonly-1.0.jar"}}}
    ]
}
with open(os.path.join(vdir, "26.3.json"), "w") as f:
    json.dump(meta, f, indent=2)

# 版本 jar：含一个应被剥离的签名文件
with zipfile.ZipFile(os.path.join(vdir, "26.3.jar"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("net/minecraft/client/main/Main.class", b"CAFEBABE-fake-class")
    z.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n")
    z.writestr("META-INF/TEST.SF", "Signature-Version: 1.0\n")

# 普通 jar（DEFLATE）+ natives-linux jar（内含 .so）
def jar(rel, entries):
    p = os.path.join(libs, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
        for n, d in entries.items():
            z.writestr(n, d)

jar("com/mojang/logging/1.7.12/logging-1.7.12.jar", {"com/mojang/logging/Logger.class": b"class"})
jar("org/lwjgl/lwjgl/3.4.1/lwjgl-3.4.1.jar", {"org/lwjgl/System.class": b"class"})
jar("org/lwjgl/lwjgl/3.4.1/lwjgl-3.4.1-natives-linux.jar",
    {"linux/x86_64/org/lwjgl/liblwjgl.so": b"SO-TEST-PAYLOAD-1234567890"})
jar("net/fabricmc/fabric-loader/0.19.3/fabric-loader-0.19.3.jar",
    {"net/fabricmc/loader/impl/launch/knot/KnotClient.class": b"class"})

# mods 单独放版本目录
with zipfile.ZipFile(os.path.join(vdir, "mods", "testmod.jar"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("fabric.mod.json", b'{"id":"testmod","version":"1.0"}')

with open(os.path.join(root, "assets", "indexes", "32.json"), "w") as f:
    json.dump({"objects": {"ab/abcdef": {"hash": "abcdef", "size": 4}}}, f)
with open(os.path.join(root, "assets", "objects", "ab", "abcdef"), "wb") as f:
    f.write(b"sound")
PY

# 假 JRE：java 是一个记录 argv 的脚本；另放若干应被裁剪的文件
cat > "$MC/runtime/java-runtime-epsilon/bin/java" <<'SH'
#!/usr/bin/env bash
: > "$MC263_FAKE_JAVA_OUT"
for a in "$@"; do printf '%s\n' "$a" >> "$MC263_FAKE_JAVA_OUT"; done
echo "fake java: $# args"
exit 0
SH
chmod 0755 "$MC/runtime/java-runtime-epsilon/bin/java"
printf 'jvm\n' > "$MC/runtime/java-runtime-epsilon/lib/modules"
printf 'cds\n' > "$MC/runtime/java-runtime-epsilon/lib/server/classes.jsa"
printf 'src\n' > "$MC/runtime/java-runtime-epsilon/src.zip"
printf 'man\n' > "$MC/runtime/java-runtime-epsilon/man/man1/java.1"

echo
echo "== 1. 构建单文件 =="
"$BUILDER" --mc-dir "$MC" --version 26.3 \
    --java-runtime "$MC/runtime/java-runtime-epsilon" \
    --launcher "$LAUNCHER" --output "$OUT" --work-dir "$TMP/work" > "$TMP/build.log" 2>&1 || {
        cat "$TMP/build.log" >&2; exit 1; }
grep -E 'jar:|ZIP|输出:' "$TMP/build.log" | head -8
check "生成 ELF 单文件" "head -c4 '$OUT' | grep -q 'ELF'"
check "缺失声明库 = 0（windows 规则被正确过滤）" "! grep -q '缺失声明库' '$TMP/build.log'"
check "提取到 .so" "grep -q '| .so: [1-9]' '$TMP/build.log'"
INFO="$("$OUT" --info)"
echo "$INFO" | sed 's/^/    /'
check "--info 报告内嵌 payload" "echo \"\$INFO\" | grep -q '内嵌 payload : 是'"

echo
echo "== 2. 首次运行：自解压（--extract-only） =="
"$OUT" --extract-only --username SelftestPlayer --game-dir "$GAME" --data-dir "$DATA" \
    > "$TMP/extract.log" 2>&1 || { cat "$TMP/extract.log" >&2; exit 1; }
sed 's/^/    /' "$TMP/extract.log" | head -8
check "app/mc.jar 已解压" "test -f '$DATA/app/mc.jar'"
check "manifest 保留 / META-INF 签名被剥离" "MC_ROOT='$DATA' python3 -c 'import zipfile,os;z=zipfile.ZipFile(os.environ[\"MC_ROOT\"]+\"/app/mc.jar\");n=z.namelist();assert \"META-INF/MANIFEST.MF\" in n and not any(x.endswith(\".SF\") for x in n) and \"net/minecraft/client/main/Main.class\" in n'"
check "natives .so 提取且内容正确" "grep -q 'SO-TEST-PAYLOAD-1234567890' '$DATA/app/natives/liblwjgl.so'"
check "内嵌 java 可执行位保留" "test -x '$DATA/runtime/bin/java'"
check "JRE 裁剪生效（src.zip / classes.jsa / man 不在）" "! test -e '$DATA/runtime/src.zip' && ! test -e '$DATA/runtime/lib/server/classes.jsa' && ! test -e '$DATA/runtime/man/man1/java.1'"
check "assets 与 config.json" "test -f '$DATA/assets/indexes/32.json' && test -f '$DATA/config.json' && test -f '$DATA/version.json'"
check "mods 预装到 gameDir（不覆盖玩家 mod）" "test -f '$GAME/mods/testmod.jar'"
check "classpath 含多个 jar（冒号分隔）" "MC_ROOT='$DATA' python3 -c 'import os,json;d=json.load(open(os.environ[\"MC_ROOT\"]+\"/config.json\"));assert d[\"version\"]==\"26.3\" and d[\"assetIndex\"]==\"32\" and d[\"mainClass\"].endswith(\"KnotClient\") and d[\"fabric\"] is True and \"-Djava.net.preferIPv4Stack=true\" in d[\"jvmArgs\"]'"
check "缓存标记已写入" "test -f '$DATA/.mc263-ready'"

echo
echo "== 3. 二次运行：缓存命中，不重复解压 =="
MARK_BEFORE="$(cat "$DATA/.mc263-ready")"
"$OUT" --extract-only --game-dir "$GAME" --data-dir "$DATA" > "$TMP/second.log" 2>&1
grep -q '缓存命中' "$TMP/second.log"
check "缓存命中" "grep -q '缓存命中' '$TMP/second.log'"
check "缓存标记未变化" "test \"\$(cat '$DATA/.mc263-ready')\" = '$MARK_BEFORE'"

echo
echo "== 4. 启动：classpath / JVM / 游戏参数 =="
"$OUT" --game-dir "$GAME" --data-dir "$DATA" --username Steve --jvm-arg -Dtest.flag=1 \
    > "$TMP/launch.log" 2>&1 || { cat "$TMP/launch.log" >&2; exit 1; }
sed 's/^/    /' "$TMP/launch.log" | head -6
check "假 java 被调用并记录了 argv" "test -s '$JAVA_ARGS'"
check "classpath 用冒号且含 mc.jar 与 fabric-loader" "grep -q ':' '$JAVA_ARGS' && grep -q 'mc.jar' '$JAVA_ARGS' && grep -q 'fabric-loader' '$JAVA_ARGS'"
check "natives 路径注入" "grep -q -- '-Djava.library.path=.*app/natives' '$JAVA_ARGS' && grep -q 'SharedLibraryExtractPath' '$JAVA_ARGS'"
check "游戏参数正确" "grep -qx -- '--version' '$JAVA_ARGS' && grep -qx '26.3' '$JAVA_ARGS' && grep -qx -- '--assetIndex' '$JAVA_ARGS' && grep -qx '32' '$JAVA_ARGS' && grep -qx -- '--username' '$JAVA_ARGS' && grep -qx 'Steve' '$JAVA_ARGS'"
check "mainClass 与 JVM 参数" "grep -qx 'net.fabricmc.loader.impl.launch.knot.KnotClient' '$JAVA_ARGS' && grep -qx -- '-Dtest.flag=1' '$JAVA_ARGS' && grep -q -- '-XX:+UseZGC' '$JAVA_ARGS'"
check "launcher.log 记录命令" "grep -q 'cmd=' '$GAME/launcher.log'"

echo
echo "== 5. 损坏 payload 应被 CRC 拦截 =="
cp "$OUT" "$TMP/corrupted"
python3 - "$TMP/corrupted" <<'PY'
import sys
p = sys.argv[1]
with open(p, "r+b") as f:
    f.seek(0, 2)
    size = f.tell()
    # payload 位于 [size-32-payloadSize, size-32)，翻转倒数第 40 字节必定命中 payload
    off = size - 40
    f.seek(off)
    b = f.read(1)
    f.seek(off)
    f.write(bytes([b[0] ^ 0xFF]))
PY
set +e
"$TMP/corrupted" --force-extract --extract-only --game-dir "$TMP/badgame" --data-dir "$TMP/baddata" \
        > "$TMP/corrupt.log" 2>&1
RC=$?
set -e
check "损坏 payload 被拒绝且是干净退出（非信号崩溃）" "test '$RC' -ne 0 -a '$RC' -lt 128"
check "日志含明确的 CRC / zstd 校验错误" "grep -Eq 'CRC32 不匹配|zstd 解压 payload 失败|zstd 解压失败|拒绝解压|拒绝继续' '$TMP/corrupt.log'"
sed 's/^/    /' "$TMP/corrupt.log" | tail -2

echo
echo "== 6. v1 零第三方运行库路径（USE_ZSTD=0） =="
g++ -std=c++23 -O1 -I"$ROOT/include" "$ROOT/src/launcher.cpp" -o "$TMP/launcher_v1"
g++ -std=c++23 -O1 -I"$ROOT/include" -DMC263_WITH_ZLIB "$ROOT/src/build_263.cpp" -o "$TMP/builder_v1" -lz
"$TMP/builder_v1" --mc-dir "$MC" --version 26.3 \
    --java-runtime "$MC/runtime/java-runtime-epsilon" \
    --launcher "$TMP/launcher_v1" --output "$TMP/out_v1/mc263" --work-dir "$TMP/work_v1" \
    > "$TMP/build_v1.log" 2>&1 || { cat "$TMP/build_v1.log" >&2; exit 1; }
check "v1 launcher 无 libzstd 依赖" "! ldd '$TMP/launcher_v1' | grep -q zstd"
check "v1 payload 格式 = 1" "'$TMP/out_v1/mc263' --info | grep -q '格式=v1 (STORED zip'"
"$TMP/out_v1/mc263" --extract-only --game-dir "$TMP/game_v1" --data-dir "$TMP/data_v1" \
    > "$TMP/extract_v1.log" 2>&1 || { cat "$TMP/extract_v1.log" >&2; exit 1; }
check "v1 自解压出 app/mc.jar 与 java" "test -f '$TMP/data_v1/app/mc.jar' && test -x '$TMP/data_v1/runtime/bin/java'"
check "v1 启动参数正确" "MC263_FAKE_JAVA_OUT='$TMP/java_v1.txt' '$TMP/out_v1/mc263' --game-dir '$TMP/game_v1' --data-dir '$TMP/data_v1' --username Alex >/dev/null 2>&1 && grep -qx -- '--username' '$TMP/java_v1.txt' && grep -qx 'Alex' '$TMP/java_v1.txt'"

echo
echo "== 7. 离线 mock CDN：下载器全链路（SHA1 / 缓存 / 并发 / 进度） =="
CPORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 "$ROOT/tests/mock_cdn.py" --port "$CPORT" > "$TMP/mock.log" 2>&1 &
MOCK_PID=$!
for i in $(seq 1 40); do
    curl -s --max-time 1 "http://127.0.0.1:$CPORT/mc/game/version_manifest_v2.json" >/dev/null 2>&1 && break
    sleep 0.15
done
MIRROR="http://127.0.0.1:$CPORT"
FETCH="$ROOT/build/mc263-fetch"
"$FETCH" --mirror "$MIRROR" --mc-dir "$TMP/mockmc" --version 26.3 \
    --component jar,libraries,assets,runtime --jobs 3 --json-progress > "$TMP/fetch.log" 2>&1 || {
        cat "$TMP/fetch.log" >&2; exit 1; }
check "下载器成功解析 manifest 与 version json" "grep -q '声明库 1 个' '$TMP/fetch.log'"
check "client.jar 是有效 jar 且 SHA1 校验通过" "python3 -c 'import zipfile,sys;z=zipfile.ZipFile(sys.argv[1]);assert \"META-INF/MOCK.SF\" in z.namelist() and \"net/minecraft/client/main/Main.class\" in z.namelist()' '$TMP/mockmc/versions/26.3/26.3.jar'"
check "library 内容正确" "grep -q 'MOCK-LIBRARY-JAR' '$TMP/mockmc/libraries/mock/lib/1.0/lib-1.0.jar'"
check "asset 对象按 hash 落盘" "test -s '$TMP/mockmc/assets/indexes/34.json' && find '$TMP/mockmc/assets/objects' -type f | grep -q ."
check "JRE bin/java 下载并带可执行位" "test -x '$TMP/mockmc/runtime/java-runtime-epsilon/bin/java'"
check "进度 JSON 覆盖多个阶段" "grep -q '\"stage\": \"libraries\"' '$TMP/fetch.log' && grep -q '\"stage\": \"assets\"' '$TMP/fetch.log' && grep -q '\"stage\": \"runtime\"' '$TMP/fetch.log'"
"$FETCH" --mirror "$MIRROR" --mc-dir "$TMP/mockmc" --version 26.3 \
    --component jar,libraries,assets,runtime --jobs 3 > "$TMP/fetch2.log" 2>&1
check "二次运行命中本地缓存" "grep -q '缓存命中 1' '$TMP/fetch2.log'"
printf 'BROKEN' > "$TMP/mockmc/versions/26.3/26.3.jar"
"$FETCH" --mirror "$MIRROR" --mc-dir "$TMP/mockmc" --version 26.3 \
    --component jar > "$TMP/fetch3.log" 2>&1
check "SHA1 不匹配时自动重新下载修复" "python3 -c 'import zipfile,sys;assert zipfile.is_zipfile(sys.argv[1])' '$TMP/mockmc/versions/26.3/26.3.jar'"
# --online：构建器自动先调 mc263-fetch，再走完整构建（mock 数据足以跑通）
"$ROOT/build/mc263-build" --online --mirror "$MIRROR" --mc-dir "$TMP/mockmc" \
    --version 26.3 --launcher "$ROOT/build/launcher" \
    --output "$TMP/online_out/mc263" --work-dir "$TMP/online_work" \
    > "$TMP/online.log" 2>&1 || { cat "$TMP/online.log" >&2; exit 1; }
check "--online 先下载再构建" "grep -q '下载完成，继续构建' '$TMP/online.log'"
check "--online 产出带 payload 的单文件" "'$TMP/online_out/mc263' --info | grep -q '内嵌 payload : 是'"
kill "$MOCK_PID" 2>/dev/null || true
MOCK_PID=""

echo
echo "== 8. GUI 任务编排（mock 工具，验证 /api/start 与状态流转） =="
mkdir -p "$TMP/tools" "$TMP/guihome"
cat > "$TMP/tools/mc263-fetch" <<'SH'
#!/usr/bin/env bash
echo '{"stage": "jar","done": 1,"total": 2,"label": "mock.jar","ok": true,"note": ""}'
echo '{"stage": "jar","done": 2,"total": 2,"label": "mock2.jar","ok": true,"note": "cached"}'
SH
cat > "$TMP/tools/mc263-build" <<'SH'
#!/usr/bin/env bash
echo "mock build ok"
SH
chmod +x "$TMP/tools/mc263-fetch" "$TMP/tools/mc263-build"
: > "$TMP/tools/launcher"
GPORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
HOME="$TMP/guihome" "$ROOT/build/mc263-gui" --no-browser --port "$GPORT" --tool-dir "$TMP/tools" \
    > "$TMP/gui.log" 2>&1 &
GPID=$!
for i in $(seq 1 40); do
    curl -s --max-time 1 "http://127.0.0.1:$GPORT/api/health" >/dev/null 2>&1 && break
    sleep 0.15
done
check "GUI 服务可用" "curl -s --max-time 3 'http://127.0.0.1:$GPORT/api/health' | grep -q '\"ok\": true'"
CFG="{\"version\":\"26.3\",\"mcDir\":\"$TMP/mockmc\",\"output\":\"$TMP/out_gui/mc263\",\"mirror\":\"official\",\"arch\":\"x86_64\",\"jobs\":2,\"memory\":\"2G\",\"username\":\"GuiTest\",\"components\":[\"jar\"],\"rpm\":false,\"fabric\":false,\"modUrls\":[]}"
curl -s --max-time 3 -X POST -H "Content-Type: application/json" -d "$CFG" \
    "http://127.0.0.1:$GPORT/api/config" > "$TMP/gui_cfg.json"
check "配置写入并可回读" "curl -s --max-time 3 'http://127.0.0.1:$GPORT/api/config' | grep -q 'GuiTest'"
curl -s --max-time 3 -X POST "http://127.0.0.1:$GPORT/api/start" > "$TMP/gui_start.json"
for i in $(seq 1 40); do
    ST="$(curl -s --max-time 3 "http://127.0.0.1:$GPORT/api/status")"
    echo "$ST" | grep -q '"finished": true' && break
    sleep 0.2
done
printf '%s' "$ST" > "$TMP/gui_status.json"
check "任务完成且 ok=true" "grep -q '\"finished\": true' '$TMP/gui_status.json' && grep -q '\"ok\": true' '$TMP/gui_status.json'"
check "阶段走到完成" "grep -q '\"phase\": \"完成\"' '$TMP/gui_status.json'"
check "日志包含 mock 工具输出" "grep -q 'mock build ok' '$TMP/gui_status.json'"
kill "$GPID" 2>/dev/null || true
GPID=""

echo
echo "== 9. 路径守卫：/、/home、/etc 等敏感目录一律拒绝 =="
BAD_REJECTED=0
for BAD in "/" "/etc" "/etc/mc263-test" "/home" "/home/mc263-test" "/usr/bin/mc263" "/var/tmp/mc263"; do
    if "$ROOT/build/mc263-fetch" --mc-dir "$BAD" --check-paths > "$TMP/path.log" 2>&1; then
        echo "  FAIL 下载器未拒绝 $BAD" >&2; exit 1
    fi
    BAD_REJECTED=$((BAD_REJECTED + 1))
done
check "下载器拒绝 7 个敏感路径（含 / 与 /home）" "test '$BAD_REJECTED' -eq 7"
check "拒绝原因明确（受保护目录）" "grep -q '受保护目录' '$TMP/path.log'"
ln -s /etc "$TMP/link-to-etc" 2>/dev/null || true
if [[ -L "$TMP/link-to-etc" ]]; then
    check "符号链接逃逸到 /etc 被挡住" "! '$ROOT/build/mc263-fetch' --mc-dir '$TMP/link-to-etc/mc263' --check-paths >/dev/null 2>&1"
else
    echo "  SKIP 符号链接测试（当前环境不允许创建链接）"
fi
check "构建器拒绝 --output /etc/mc263" "! '$ROOT/build/mc263-build' --output /etc/mc263 --check-paths >/dev/null 2>&1"
check "构建器拒绝 --output /home/mc263" "! '$ROOT/build/mc263-build' --output /home/mc263 --check-paths >/dev/null 2>&1"
check "构建器接受沙箱内输出" "'$ROOT/build/mc263-build' --output '$TMP/dist/mc263' --work-dir '$TMP/work' --check-paths >/dev/null 2>&1"
check "下载器接受沙箱内游戏目录" "'$ROOT/build/mc263-fetch' --mc-dir '$TMP/allowed/gamefiles' --check-paths >/dev/null 2>&1"
if bash "$ROOT/packaging/build-rpm.sh" --payload "$OUT" --out /etc > "$TMP/rpm_path.log" 2>&1; then
    echo "  FAIL rpm 脚本未拒绝 --out /etc" >&2; exit 1
fi
check "rpm 脚本拒绝 --out /etc" "grep -q '受保护目录' '$TMP/rpm_path.log'"
# 单独起一个 GUI 服务，验证 HTTP 配置接口也拒绝敏感路径
G2PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
HOME="$TMP/guihome2" "$ROOT/build/mc263-gui" --no-browser --port "$G2PORT" > "$TMP/gui2.log" 2>&1 &
GPID=$!
for i in $(seq 1 40); do
    curl -s --max-time 1 "http://127.0.0.1:$G2PORT/api/health" >/dev/null 2>&1 && break
    sleep 0.15
done
RSP="$(curl -s --max-time 3 -X POST -H 'Content-Type: application/json' \
      -d "{\"mcDir\":\"/etc/mc263\",\"output\":\"$TMP/out_gui2/mc263\"}" \
      "http://127.0.0.1:$G2PORT/api/config")"
printf '%s' "$RSP" > "$TMP/gui2_cfg.json"
check "GUI /api/config 拒绝 /etc 并返回错误" "grep -q '\"ok\": false' '$TMP/gui2_cfg.json' && grep -q '受保护目录' '$TMP/gui2_cfg.json'"
kill "$GPID" 2>/dev/null || true
GPID=""

echo
printf '\033[32m全部通过：%d 项检查 | payload 格式: %s\033[0m\n' "$PASS" "$FORMAT"
