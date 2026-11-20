// core_test.cpp -- launcher_core.hpp 的离线单测（不依赖 Qt、不联网）
#include "launcher_core.hpp"
#include "modrinth.hpp"

#include <chrono>
#include <cstdlib>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace mc263::core;

static int passed = 0;
static void check(bool ok, const std::string& name) {
    if (ok) { std::cout << "  PASS " << name << "\n"; ++passed; }
    else { std::cout << "  FAIL " << name << "\n"; std::exit(1); }
}

static const char* kFakeVersion = R"({
  "id": "26.3",
  "mainClass": "net.minecraft.client.main.Main",
  "assetIndex": {"id": "34"},
  "javaVersion": {"majorVersion": 25},
  "libraries": [
    {"name": "mock:lib:1.0", "downloads": {"artifact": {"path": "mock/lib/1.0/lib-1.0.jar"}}},
    {"name": "mock:native:1.0", "rules": [{"action": "allow", "os": {"name": "linux"}}],
     "downloads": {"classifiers": {"natives-linux": {"path": "mock/native/1.0/native-1.0-natives-linux.jar"}}}},
    {"name": "mock:winonly:1.0", "rules": [{"action": "allow", "os": {"name": "windows"}}],
     "downloads": {"artifact": {"path": "mock/win/1.0/win-1.0.jar"}}}
  ],
  "downloads": {"client": {"url": "https://example.invalid/client.jar"}}
})";

int main() {
    fs::path root = fs::absolute("build/coretest");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // 1) 布局与路径守卫
    Layout l = Layout::detect(root / "mc263-qt", "");
    check(l.appDir == root, "布局：应用目录 = ELF 所在目录");
    check(l.gameDir == root / "minecraft", "布局：游戏目录在应用目录下");
    check(l.configDir == root / ".mc263", "布局：配置目录为 .mc263");
    std::string err;
    check(l.ensure(err), "目录创建成功且通过路径守卫：" + err);
    check(fs::is_directory(l.versionsDir) && fs::is_directory(l.configDir), "子目录已建立");

    // 2) 配置往返
    Config c;
    c.version = "26.3"; c.username = "Steve"; c.memoryMb = 6144; c.minMemoryMb = 1024;
    c.mirror = "bmclapi"; c.fabric = true; c.jobs = 12;
    c.gameDir = (root / "gd").string();
    c.extraJvmArgs = {"-Dtest.flag=1"};
    c.components = {"jar", "assets"};
    check(c.save(l.configFile, err), "配置保存：" + err);
    Config c2;
    check(c2.load(l.configFile, err), "配置读取：" + err);
    check(c2.username == "Steve" && c2.memoryMb == 6144 && c2.minMemoryMb == 1024,
          "配置字段往返一致");
    check(c2.fabric && c2.jobs == 12 && c2.mirror == "bmclapi", "布尔与数值字段一致");
    check(c2.gameDir == c.gameDir, "游戏目录可持久化（实时保存靠它）");
    check(Config{}.jobs == 10, "并发线程默认 10");
    check(c2.components.size() == 2 && c2.extraJvmArgs.size() == 1, "数组字段一致");
    check(c2.xmxString() == "-Xmx6144M" && c2.xmsString() == "-Xms1024M", "Xmx/Xms 生成正确");
    {
        Config dc;
        dc.version = "26.3";
        dc.profile = "fabric";
        check(versionDirName(dc) == "26.3+fabric", "版本目录名自动追加加载器");
        dc.version = "26.3+fabric";
        check(versionDirName(dc) == "26.3+fabric", "完整目录名不再重复追加加载器");
    }

    // 3) 假版本 json
    fs::path vdir = l.versionDir("26.3");
    fs::create_directories(vdir, ec);
    { std::ofstream o(l.versionJson("26.3")); o << kFakeVersion; }
    Config vc; vc.arch = "x86_64";
    VersionMeta vm = loadVersion(l, vc);
    check(vm.ok(), "版本 json 解析：" + vm.error);
    check(vm.mainClass == "net.minecraft.client.main.Main" && vm.assetIndex == "34" && vm.javaMajor == 25,
          "主类 / assetIndex / Java 版本提取正确");
    check(vm.classpath.size() == 2, "windows-only 库被 rules 过滤，classpath 2 项");
    bool hasWin = false;
    for (const auto& p : vm.classpath) if (p.find("winonly") != std::string::npos) hasWin = true;
    check(!hasWin, "classpath 不含 windows 专属库");
    check(vm.nativeJars.size() == 1, "natives-linux classifier 已选中");

    // 4) 兼容性查验
    std::vector<JavaInfo> empty;
    auto items = compatibility(l, vc, empty, false);
    check(items.size() >= 9, "兼容性检查覆盖 9 项以上");
    bool pathOk = false, javaBad = false;
    for (const auto& it : items) {
        if (it.name == "路径安全" && it.ok) pathOk = true;
        if (it.name == "Java 运行时" && !it.ok) javaBad = true;
    }
    check(pathOk, "路径安全项通过");
    check(javaBad, "未提供 Java 时兼容性检查会报缺失");

    // 5) 启动命令拼装（用 /bin/echo 作为假 java，不真正执行）
    JavaInfo fake; fake.path = "/bin/echo"; fake.major = 25; fake.valid = true; fake.version = "25.0.1";
    LaunchPlan plan = buildLaunchPlan(l, vc, fake);
    check(plan.ok(), "启动命令生成：" + plan.error);
    check(plan.argv[0] == "/bin/echo" && plan.classpath.find(':') != std::string::npos,
          "argv[0] 与冒号分隔 classpath 正确");
    bool hasXmx = false, hasCp = false, hasLibPath = false, hasUsername = false, hasMain = false;
    for (size_t i = 0; i < plan.argv.size(); ++i) {
        if (plan.argv[i] == "-Xmx4096M") hasXmx = true;
        if (plan.argv[i] == "-cp") hasCp = true;
        if (plan.argv[i].rfind("-Djava.library.path=", 0) == 0) hasLibPath = true;
        if (plan.argv[i] == "--username") hasUsername = true;
        if (plan.argv[i] == vm.mainClass) hasMain = true;
    }
    bool hasLang = false;
    std::string langVal;
    for (size_t i = 0; i + 1 < plan.argv.size(); ++i)
        if (plan.argv[i] == "--language") { hasLang = true; langVal = plan.argv[i + 1]; }
    check(hasLang && !langVal.empty(), "启动参数带 --language");
    check(!hasXmx && hasCp && hasLibPath && hasUsername && hasMain,
          "JVM 自动内存：默认不传 -Xmx");
    vc.autoMemory = false;
    LaunchPlan manual = buildLaunchPlan(l, vc, fake);
    bool manualXmx = false;
    for (const auto& a : manual.argv) if (a == "-Xmx4096M") manualXmx = true;
    check(manualXmx, "手动内存：-Xmx 按配置传入");
    vc.autoMemory = true;
    check(plan.workDir == l.versionsDir / "26.3", "版本隔离：工作目录 = versions/<版本>");
    check(std::filesystem::is_directory(plan.workDir / "mods"), "版本隔离：自动建 mods 目录");

    // 5.1) 系统语言 → Minecraft 语言代码
    ::setenv("LC_ALL", "zh_CN.UTF-8", 1);
    ::setenv("LANG", "zh_CN.UTF-8", 1);
    check(detectSystemLanguage() == "zh_cn", "系统语言 zh_CN → zh_cn");
    ::setenv("LC_ALL", "en_US.UTF-8", 1);
    check(detectSystemLanguage() == "en_us", "系统语言 en_US → en_us");
    ::setenv("LC_ALL", "de_DE.UTF-8", 1);
    check(detectSystemLanguage() == "de_de", "系统语言 de_DE → de_de");
    ::setenv("LC_ALL", "zh_TW.UTF-8", 1);
    check(detectSystemLanguage() == "zh_tw", "系统语言 zh_TW → zh_tw");
    ::setenv("LC_ALL", "C", 1);
    ::setenv("LANG", "C", 1);
    check(detectSystemLanguage() == "en_us", "系统语言 C/POSIX → 退回 en_us");
    ::unsetenv("LC_ALL");
    ::unsetenv("LANG");

    // 6) 本地版本列表
    auto vers = availableVersions(l);
    check(vers.size() == 1 && vers[0] == "26.3", "本地版本列表扫描正确");

    // 6.1) Modrinth 响应解析（离线，用固定 JSON）
    const std::string searchJson = R"({"hits":[
        {"project_id":"AABBCC","slug":"sodium","title":"Sodium","author":"jellysquid3",
         "description":"现代渲染优化","downloads":12345678},
        {"project_id":"DDEEFF","slug":"lithium","title":"Lithium","author":"jellysquid3",
         "description":"服务端/客户端逻辑优化","downloads":9876543}]})";
    std::vector<mc263::modrinth::Hit> hits;
    std::string merr;
    check(mc263::modrinth::parseSearch(searchJson, hits, merr) && hits.size() == 2 &&
              hits[0].id == "AABBCC" && hits[0].title == "Sodium" &&
              hits[0].downloads == 12345678,
          "Modrinth 搜索响应解析");

    const std::string verJson = R"([{"id":"ver1","name":"Sodium 0.6","version_number":"0.6.0",
        "version_type":"release","game_versions":["26.3"],"loaders":["fabric"],
        "files":[{"url":"https://cdn/x.jar","filename":"sodium-0.6.jar","size":2048,
                  "primary":true,"hashes":{"sha1":"abc123"}}]}])";
    std::vector<mc263::modrinth::Version> mvers;
    check(mc263::modrinth::parseVersions(verJson, mvers, merr) && mvers.size() == 1 &&
              mvers[0].gameVersions.size() == 1 && mvers[0].loaders[0] == "fabric" &&
              mvers[0].versionType == "release" &&
              mc263::modrinth::primaryFile(mvers[0]) != nullptr &&
              mc263::modrinth::primaryFile(mvers[0])->sha1 == "abc123",
          "Modrinth 版本响应解析");
    check(mc263::modrinth::urlEncode("a b/c") == "a%20b%2Fc", "Modrinth 查询参数转义");

    // 6.2) JSON 大对象：解析不应退化成 O(n²)，重复 key 仍后值覆盖
    {
        std::string big = R"({"objects":{)";
        big.reserve(20000 * 32);
        for (int i = 0; i < 20000; ++i) {
            if (i) big += ',';
            big += "\"k" + std::to_string(i) + "\":{\"hash\":\"h" + std::to_string(i) + "\"}";
        }
        big += "}}";

        std::string jerr;
        const auto t0 = std::chrono::steady_clock::now();
        JValue j = mc263::jsonParse(big, jerr);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        const JValue* objs = j.find("objects");
        check(jerr.empty() && objs && objs->object().size() == 20000,
              "JSON：2 万 key 对象解析完整");
        check(ms < 5000, "JSON：2 万 key 解析没有退化成 O(n²)（" +
                             std::to_string(ms) + " ms）");

        JValue dup = mc263::jsonParse(R"({"a":1,"a":2,"b":3})", jerr);
        const JValue* a = dup.find("a");
        check(jerr.empty() && a && a->number() == 2 && dup.object().size() == 2,
              "JSON：重复 key 仍然后值覆盖");
    }

    // 6.3) 元数据临时路径：同 URL 连续生成也必须唯一
    {
        std::set<fs::path> paths;
        for (int i = 0; i < 1000; ++i)
            paths.insert(mc263::dl::metaTempPath("/tmp/mc263-cache", "https://example.invalid/x"));
        check(paths.size() == 1000, "下载：临时文件名并发唯一");
    }

    {
        fs::create_directories(l.versionDir("26.3+fabric"), ec);
        std::ofstream o(l.versionJson("26.3+fabric"));
        o << R"({"libraries":[{"name":"net.fabricmc:fabric-loader:0.16.9"}]})";
        o.close();
        check(installedLoaderVersion(l, "26.3", "fabric") == "0.16.9",
              "已装加载器版本可从版本 json 读取");
    }
    // 装到 /usr/bin 时改用 XDG 数据目录：at() 必须把 root 当作应用目录
    Layout installed = Layout::at(fs::path("/tmp/xdg-mc263-test"));
    check(installed.appDir == "/tmp/xdg-mc263-test" &&
              installed.configFile == "/tmp/xdg-mc263-test/.mc263/config.json" &&
              installed.gameDir == "/tmp/xdg-mc263-test/minecraft",
          "Layout::at 支持自定义应用目录（安装到只读路径时回落）");

    // 7) 显示后端选择（Wayland 优先，缺插件回退 XWayland，全缺则报错）
    check(choosePlatform("offscreen", true, true, true).plugin == "offscreen",
          "显式 QT_QPA_PLATFORM 一律尊重");
    check(choosePlatform("", true, true, true).plugin == "wayland",
          "Wayland 会话 + wayland 插件 → 原生 wayland");
    auto fallback = choosePlatform("", true, false, true);
    check(fallback.plugin == "xcb" && !fallback.fatal,
          "Wayland 会话缺 wayland 插件 → 回退 xcb（XWayland）");
    check(choosePlatform("", true, false, false).fatal,
          "Wayland 会话两个插件都没有 → 明确报错而不是让 Qt qFatal");
    check(choosePlatform("", false, true, true).plugin == "xcb",
          "X11 会话优先 xcb");
    check(choosePlatform("", false, true, false).plugin == "wayland",
          "未识别为 Wayland 会话且只有 wayland 插件 → 仍可启动");

    // 8) /proc/meminfo 口径：程序占用 / 缓存 / 仅可用
    const char* kMem = R"(MemTotal:       32732120 kB
MemFree:         1048576 kB
MemAvailable:   16777216 kB
Buffers:          262144 kB
Cached:         11534336 kB
SReclaimable:     524288 kB
Shmem:            262144 kB
HugePages_Total:       0
)";
    MemInfo mem = parseMemInfo(kMem);
    check(mem.totalMb == 31964, "内存：总量 31964 MB（32732120 kB）");
    check(mem.availableMb == 16384, "内存：仅可用 16384 MB（MemAvailable）");
    check(mem.usedMb == 15580, "内存：程序占用 = total - available");
    check(mem.cachedMb == 11520, "内存：缓存 = Cached + SReclaimable - Shmem");
    const char* kOld = R"(MemTotal:       8192000 kB
MemFree:         524288 kB
Cached:          2097152 kB
)";
    MemInfo old = parseMemInfo(kOld);
    check(old.availableMb == old.freeMb + old.cachedMb,
          "内存：无 MemAvailable 的老内核用 free + cache 估算");
    MemInfo live = readMemInfo();
    check(live.totalMb > 0 && live.availableMb > 0 && live.availableMb <= live.totalMb,
          "内存：本机 /proc/meminfo 可读且口径自洽");

    // 9) 各版本要求的 Java 主版本（26.x=25，1.20+=21，1.17~1.19=17，更早=8）
    check(javaMajorForVersionName("26.3") == 25, "Java 要求：26.3 → 25");
    check(javaMajorForVersionName("1.21.4") == 21, "Java 要求：1.21.4 → 21");
    check(javaMajorForVersionName("1.20.1") == 21, "Java 要求：1.20.1 → 21");
    check(javaMajorForVersionName("1.17.1") == 17, "Java 要求：1.17.1 → 17");
    check(javaMajorForVersionName("1.12.2") == 8, "Java 要求：1.12.2 → 8");
    check(requiredJavaMajor(l, "26.3") == 25, "Java 要求：有 version json 时读 javaVersion.majorVersion");
    check(requiredJavaMajor(l, "1.20.1") == 21, "Java 要求：没有 json 时按版本号推断");

    // 9.5) Modrinth 版本里的前置（dependencies）解析
    {
        const std::string kVer = R"([{
            "id": "ver-1", "version_number": "1.2.3", "name": "demo",
            "game_versions": ["1.21.1"], "loaders": ["fabric"],
            "files": [{"url": "https://example.invalid/a.jar", "filename": "a.jar",
                       "hashes": {"sha1": "abc"}, "primary": true}],
            "dependencies": [
                {"version_id": "depv", "project_id": "P1", "dependency_type": "required"},
                {"project_id": "P2", "dependency_type": "optional"},
                {"file_name": "some-non-modrinth-mod.jar", "dependency_type": "required"}
            ]
        }])";
        std::vector<mc263::modrinth::Version> vs;
        std::string err;
        check(mc263::modrinth::parseVersions(kVer, vs, err) && vs.size() == 1,
              "Modrinth：版本数组解析");
        check(vs[0].dependencies.size() == 3, "Modrinth：dependencies 全部保留");
        check(vs[0].dependencies[0].projectId == "P1" && vs[0].dependencies[0].versionId == "depv" &&
                  vs[0].dependencies[0].type == "required",
              "Modrinth：required 前置带 project/version id");
        check(vs[0].dependencies[2].projectId.empty() &&
                  vs[0].dependencies[2].fileName == "some-non-modrinth-mod.jar",
              "Modrinth：只有 file_name 的前置（非 Modrinth 项目）能识别");
        check(mc263::modrinth::primaryFile(vs[0]) != nullptr &&
                  mc263::modrinth::primaryFile(vs[0])->filename == "a.jar",
              "Modrinth：主文件选择");
    }

    // 10) Noctalia 色板解析 / 路径搜索 / niri 会话识别
    {
        const std::string kPal = R"(
# comment line
primary = "#004890"
on_primary = "#ffffff"
background = "#f9f9ff"
on_surface = "#191c21"
surface_container_low = "#f2f3fb"
broken = "not-a-color"
)";
        M3Palette pal = parseM3Palette(kPal, "/tmp/palette.toml");
        check(pal.valid(), "色板：background / on_surface / primary 齐了才算有效");
        check(pal.get("primary") == "#004890", "色板：键值解析");
        check(pal.get("outline", "#000000") == "#000000", "色板：缺字段走回退值");
        check(pal.colors.find("broken") == pal.colors.end(), "色板：非法颜色被丢弃");
        check(!pal.dark(), "色板：浅色背景判为亮色");

        M3Palette darkPal = parseM3Palette(
            "background = \"#101318\"\non_surface = \"#e4e6eb\"\nprimary = \"#a8c7fa\"\n");
        check(darkPal.dark(), "色板：深色背景判为暗色");
        check(!parseM3Palette("primary = \"#004890\"\n").valid(), "色板：缺背景/正文时不算有效");

        setenv("MC263_PALETTE", "/tmp/mc263-palette-test.toml", 1);
        const auto cands = paletteCandidates();
        check(!cands.empty() && cands.front() == "/tmp/mc263-palette-test.toml",
              "色板：MC263_PALETTE 覆盖默认搜索路径");
        unsetenv("MC263_PALETTE");

        setenv("NIRI_SOCKET", "/run/user/1000/niri.test.sock", 1);
        check(isNiriSession(), "会话：NIRI_SOCKET 存在 → niri");
        unsetenv("NIRI_SOCKET");
        setenv("XDG_CURRENT_DESKTOP", "sway", 1);
        setenv("XDG_SESSION_DESKTOP", "sway", 1);
        check(!isNiriSession(), "会话：非 niri 桌面名 → 不是 niri");
        setenv("XDG_CURRENT_DESKTOP", "niri", 1);
        check(isNiriSession(), "会话：XDG_CURRENT_DESKTOP=niri → niri");
    }

    // 11) 只读安装（ELF 在 /usr/bin）时的布局：数据根必须当沙箱根
    {
        std::string err;
        Layout data = Layout::at("/tmp/mc263-layout-test");
        check(data.writable(err), "布局：数据根自身可写");
        Layout same =
            Layout::at("/tmp/mc263-layout-test", "/tmp/mc263-layout-test/minecraft");
        check(same.writable(err), "布局：游戏目录在数据根之下时可写");
        Layout bad =
            Layout::detect("/usr/bin/mc263-qt", "/tmp/mc263-layout-test/minecraft");
        check(!bad.writable(err), "布局：拿 /usr/bin 当沙箱根会被拒（修复前就踩这个坑）");
        Layout homePick = Layout::at("/tmp/mc263-layout-test", "/home/bus/Games/mc263");
        check(homePick.writable(err), "布局：用户显式选在 ~ 下的游戏目录允许写入");
        Layout danger = Layout::at("/tmp/mc263-layout-test", "/etc/mc263");
        check(!danger.writable(err), "布局：显式选 /etc 仍然拒绝");
    }

    // 12) 加载器版本（26.3+fabric）要用父版本的 client jar
    {
        std::string e;
        fs::create_directories(l.versionDir("26.3+fabric"), ec);
        { std::ofstream o(l.versionJson("26.3+fabric")); o << kFakeVersion; }
        { std::ofstream o(l.versionJar("26.3")); o << "fake-jar"; }   // 父版本 jar

        Config lc;
        lc.version = "26.3";
        lc.profile = "fabric";
        lc.arch = "x86_64";
        VersionMeta lvm = loadVersion(l, lc);
        check(lvm.ok(), "加载器版本：json 能解析 " + lvm.error);
        bool hasParentJar = false, hasOwnJar = false;
        for (const auto& p : lvm.classpath) {
            if (p.find("26.3/26.3.jar") != std::string::npos) hasParentJar = true;
            if (p.find("26.3+fabric/26.3+fabric.jar") != std::string::npos) hasOwnJar = true;
        }
        check(hasParentJar && !hasOwnJar, "加载器版本：classpath 用父版本 jar，不塞不存在的自身 jar");

        Config noParent;
        noParent.version = "1.21.9";   // 没有这个版本
        noParent.profile = "fabric";
        noParent.arch = "x86_64";
        VersionMeta miss = loadVersion(l, noParent);
        check(!miss.ok() && miss.error.find("缺少版本 json") != std::string::npos,
              "加载器版本：父版本缺失时报缺 json");
    }

    // 13) 本地文件管理：mods 启用/禁用、启动日志保留
    {
        std::string e;
        fs::path d = root / "ent";
        fs::create_directories(d / "world", ec);
        { std::ofstream o(d / "a.jar"); o << "jar"; }
        { std::ofstream o(d / "b.jar.disabled"); o << "jar"; }

        auto list = listEntries(d);
        check(list.size() == 3, "文件管理：列出 3 项（含子目录）");
        std::string bName;
        bool bDisabled = false;
        for (const auto& x : list)
            if (x.name == "b.jar") { bName = x.name; bDisabled = !x.enabled; }
        check(bName == "b.jar" && bDisabled, "文件管理：.disabled 显示为禁用且名称去掉后缀");

        fs::path bPath = d / "b.jar.disabled";
        check(setEntryEnabled(bPath, true, e), "文件管理：启用（去 .disabled）");
        check(fs::exists(d / "b.jar") && !fs::exists(bPath), "文件管理：启用后文件已改名");
        check(setEntryEnabled(d / "b.jar", false, e), "文件管理：禁用（加 .disabled）");
        check(fs::exists(bPath), "文件管理：禁用后文件带 .disabled");

        fs::path note = root / "note";
        fs::path logPath = newLogPath(note, "26.3+fabric", e);
        check(!logPath.empty() && fs::is_directory(note), "日志：note 目录会创建");
        check(logPath.filename().string().rfind("launch-26.3-fabric-", 0) == 0,
              "日志：文件名含版本标签与时间");
        for (int i = 0; i < 12; ++i) { std::ofstream o(note / ("x" + std::to_string(i) + ".log")); o << i; }
        int removed = pruneLogs(note, 10);
        int left = 0;
        for (const auto& f : fs::directory_iterator(note))
            if (f.path().extension() == ".log") ++left;
        check(removed == 2 && left == 10, "日志：只保留最近 10 个");
    }

    // 14) 回收站：移入 → 列出 → 恢复 → 清空
    {
        std::string e;
        const fs::path trash = root / "trash";
        const fs::path saves = root / "saves";
        fs::create_directories(saves / "My World" / "region", ec);
        { std::ofstream o(saves / "My World" / "level.dat"); o << "level-data"; }

        fs::path moved;
        check(moveToTrash(saves / "My World", trash, moved, e), "回收站：世界移入成功 " + e);
        check(!fs::exists(saves / "My World"), "回收站：原位置已清空");
        auto items = listTrash(trash);
        check(items.size() == 1 && items[0].isDir && items[0].name == "My World",
              "回收站：能列出目录条目");
        check(items[0].original == saves / "My World", "回收站：记录了原始路径");
        check(items[0].size >= 10, "回收站：统计出条目大小");

        check(restoreTrash(items[0], e), "回收站：恢复成功 " + e);
        check(fs::exists(saves / "My World" / "level.dat"), "回收站：文件回到原处");
        check(listTrash(trash).empty(), "回收站：恢复后条目消失");

        // 原位置被占用时恢复，不能覆盖
        check(moveToTrash(saves / "My World", trash, moved, e), "回收站：再次移入");
        fs::create_directories(saves / "My World", ec);
        { std::ofstream o(saves / "My World" / "level.dat"); o << "new-level"; }
        auto again = listTrash(trash);
        check(again.size() == 1 && restoreTrash(again[0], e), "回收站：同名时恢复不覆盖 " + e);
        check(fs::exists(saves / "My World" / "level.dat") &&
                  fs::exists(saves / "My World (2)" / "level.dat"),
              "回收站：重名自动加序号，两份都在");

        // 已经在回收站里的东西不能再套一层
        check(moveToTrash(saves / "My World (2)", trash, moved, e), "回收站：第三次移入");
        auto third = listTrash(trash);
        check(third.size() == 1 && !moveToTrash(third[0].payload, trash, moved, e),
              "回收站：拒绝把回收站里的东西再移进去");

        uint64_t freed = 0;
        const int n = emptyTrash(trash, freed);
        check(n > 0 && listTrash(trash).empty(), "回收站：清空后没有条目");
        check(freed >= 10, "回收站：清空后报告释放字节");
    }

    // 15) 崩溃摘要：拼接本次日志 / latest.log / hs_err / crash-report
    {
        std::string e;
        Layout cl = Layout::at(root / "crash", "");
        cl.ensure(e);
        const auto vdir = cl.versionDir("26.3");
        fs::create_directories(vdir / "logs", ec);
        fs::create_directories(vdir / "crash-reports", ec);
        { std::ofstream o(vdir / "logs" / "latest.log"); o << "line-a\nline-b\n"; }
        { std::ofstream o(vdir / "hs_err_pid123.log"); o << "SIGSEGV at foo\n"; }
        { std::ofstream o(vdir / "crash-reports" / "crash-2026-11-16.txt"); o << "Description: crash\n"; }
        const fs::path launchLog = cl.gameDir / "note" / "launch-x.log";
        fs::create_directories(cl.gameDir / "note", ec);
        { std::ofstream o(launchLog); o << "launcher line\n"; }

        const auto rep = collectCrashReport(cl, "26.3", launchLog, 1, e);
        check(!rep.empty() && fs::is_regular_file(rep), "崩溃摘要：文件已生成 " + e);
        std::string rerr;
        const std::string text = fsx::readText(rep, rerr);
        check(text.find("退出码 : 1") != std::string::npos, "崩溃摘要：包含退出码");
        check(text.find("launcher line") != std::string::npos, "崩溃摘要：包含本次启动日志");
        check(text.find("line-b") != std::string::npos, "崩溃摘要：包含 latest.log");
        check(text.find("SIGSEGV at foo") != std::string::npos, "崩溃摘要：包含 hs_err");
        check(text.find("Description: crash") != std::string::npos, "崩溃摘要：包含 crash-report");

        std::string many;
        for (int i = 0; i < 300; ++i) many += "l" + std::to_string(i) + "\n";
        const std::string tail = tailLines(many, 10);
        check(tail.find("l299\n") != std::string::npos && tail.find("l288\n") == std::string::npos,
              "崩溃摘要：只留最后 N 行");
    }

    // 游戏显示后端配置往返
    {
        Config dc;
        dc.gameDisplay = "wayland";
        Config back = Config::fromJson(dc.toJson());
        check(back.gameDisplay == "wayland", "配置：gameDisplay 可往返");
        check(Config{}.gameDisplay == "auto", "配置：gameDisplay 默认 auto");
    }

    // 按版本隔离的 JVM / 显示设置
    {
        Config vc;
        VersionOptions opt;
        opt.extraJvmArgs = {"-Dfoo=1", "-XX:+UseG1GC"};
        opt.language = "zh_cn";
        opt.gameDisplay = "wayland";
        opt.appliedLanguage = "zh_cn";
        vc.versionOptions["26.3+fabric"] = opt;
        Config back = Config::fromJson(vc.toJson());
        const auto it = back.versionOptions.find("26.3+fabric");
        check(it != back.versionOptions.end() && it->second.extraJvmArgs.size() == 2 &&
                  it->second.language == "zh_cn" && it->second.gameDisplay == "wayland" &&
                  it->second.appliedLanguage == "zh_cn",
              "配置：按版本的 JVM/显示设置可往返");
    }

    // 加载器目录被删/未装时退回原版
    {
        Layout ll = Layout::at(root / "launcher");
        std::string e;
        ll.ensure(e);
        fs::create_directories(ll.versionDir("26.3"), ec);
        { std::ofstream o(ll.versionJson("26.3")); o << kFakeVersion; }
        Config cc;
        cc.version = "26.3";
        cc.profile = "quilt";
        cc.arch = "x86_64";
        check(effectiveVersionId(ll, cc) == "26.3", "版本目录：没有 +quilt 时退回原版");
        check(isolatedGameDir(ll, cc).filename().string() == "26.3", "版本目录：隔离目录也退回原版");

        fs::create_directories(ll.versionDir("26.3+quilt"), ec);
        { std::ofstream o(ll.versionJson("26.3+quilt")); o << kFakeVersion; }
        check(effectiveVersionId(ll, cc) == "26.3+quilt", "版本目录：有 +quilt 时用加载器版本");

        Config missing;
        missing.version = "26.4-snapshot-8";
        missing.profile = "quilt";
        missing.arch = "x86_64";
        check(effectiveVersionId(ll, missing) == "26.4-snapshot-8+quilt",
              "版本目录：两个都没有时返回原名（报错能指到具体文件）");
    }

    // 游戏语言写进 options.txt
    {
        std::string e;
        fs::path gd = root / "langtest";
        fs::create_directories(gd, ec);
        { std::ofstream o(gd / "options.txt"); o << "fov:70\nlang:en_us\nfoo:1\n"; }
        check(applyGameLanguage(gd, "zh_cn", e), "语言：写入 options.txt 成功");
        const std::string text = fsx::readText(gd / "options.txt", e);
        check(text.find("lang:zh_cn") != std::string::npos, "语言：lang 行被改成 zh_cn");
        check(text.find("fov:70") != std::string::npos && text.find("foo:1") != std::string::npos,
              "语言：其他设置行原样保留");
        check(text.find("lang:en_us") == std::string::npos, "语言：不再有 en_us");

        fs::path fresh = root / "langfresh";
        check(applyGameLanguage(fresh, "ja_jp", e), "语言：没有 options.txt 时直接创建");
        check(fsx::readText(fresh / "options.txt", e).find("lang:ja_jp") != std::string::npos,
              "语言：新文件里是 ja_jp");

        Config lc;
        lc.language = "en_us";
        check(effectiveGameLanguage(lc) == "en_us", "语言：显式设置优先");
        lc.language.clear();
        check(effectiveGameLanguage(lc) == detectSystemLanguage(), "语言：空则跟随系统");
    }

    std::cout << "core: " << passed << " 项通过\n";
    return 0;
}