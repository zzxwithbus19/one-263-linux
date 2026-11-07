// build_263.cpp -- build_262.py 的 C++ 重写（Linux / Minecraft 26.3 单文件版）
//
// 与 Python 原版的对应关系：
//   shutil.copytree(JAVA_RT)        -> copyRuntime()      （Linux JRE + 裁剪）
//   zipfile 剥离 262+fo.jar 签名    -> repackVersionJar()
//   硬编码 26.2 库表               -> collectLibraries() （从 26.3 version json 动态解析）
//   提取 -natives-windows.jar 的 dll-> extractNatives()  （-natives-linux*.jar 的 .so）
//   copy mods / assets             -> copyMods() / copyAssets()
//   C# 模板 + .csproj + dotnet     -> writeConfig() + 预编译的 launcher ELF
//   262_embed.zip + 内嵌资源       -> createPayload() + assemble()（追加到 ELF 尾部）
//
// 用法：mc263-build --mc-dir ~/.minecraft --version 26.3 [更多选项]
// 编译：见 Makefile（需要 -DMC263_WITH_ZLIB -lz，仅构建机依赖 zlib）
#include "fs_util.hpp"
#include "mini_json.hpp"
#include "mini_zip.hpp"
#include "safe_path.hpp"
#include "sfx_format.hpp"
#ifdef MC263_WITH_ZSTD
#include "zstd_stream.hpp"
#endif

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using mc263::JValue;
using mc263::fsx::endsWith;

static const char* kUsage = R"(mc263-build -- 把 Minecraft 26.3 (Linux) 打包成一键启动的单文件

用法: mc263-build [选项]

来源:
  --mc-dir PATH          Minecraft 数据目录        (默认 $MC263_MC_DIR 或 ~/.minecraft)
  --version VER          版本目录名                (默认 $MC263_VERSION 或 26.3)
  --version-dir PATH     直接指定 versions/<ver> 目录（覆盖上面两项）
  --java-runtime PATH    Linux JRE 根目录，含 bin/java（默认自动探测）
  --arch ARCH            目标架构 x86_64 / aarch64（默认本机 uname -m）
  --mods-dir PATH        额外的 mods 目录（可重复）
  --extra-lib PATH       额外加入 classpath 的 jar（可重复）
  --all-libs             打包 libraries/ 下全部 jar（不依赖 version json 的保底模式）
  --online               构建前自动调用 mc263-fetch 下载缺失文件（一步到位）
  --mirror NAME          配合 --online：official | bmclapi | http://自定义镜像
  --jobs N               配合 --online：并发下载数 1-16（默认 8）

产物:
  --launcher PATH        C++ 启动器 ELF           (默认 ./build/launcher)
  --output PATH          输出单文件               (默认 ./dist/mc263)
  --work-dir PATH        暂存目录                 (默认 ./build/work)
  --rpm                  打包完成后再生成 .rpm（调用 packaging/build-rpm.sh）

调优:
  --extra-jvm-arg ARG    追加 JVM 参数（可重复）
  --username NAME        默认玩家名（运行时可用 argv/环境变量覆盖）
  --keep-signatures      不剥离版本 jar 的 META-INF 签名
  --no-natives-scan      不额外扫描 libraries 里的 natives-linux jar
  --preserve-work        保留暂存目录（默认保留，便于排查）
  --verbose              打印每个文件
  --check-paths          只校验输出路径是否合法，然后退出（不写文件）
  --help                显示本帮助

路径安全: 写入目标（--output / --work-dir / rpm 输出）默认只允许项目目录（沙箱根）；
         /、/home、/etc、/usr 等一律拒绝。只读来源（--mc-dir、--java-runtime）不受限制。
         MC263_SANDBOX_ROOT 改沙箱根，MC263_ALLOW_OUTSIDE=1 放行沙箱外非敏感路径。

示例（输出都落在项目目录内）:
  mc263-build --mc-dir gamefiles --version 26.3 --output dist/mc263
  mc263-build --online --version 26.3 --output dist/mc263 --rpm
  mc263-build --check-paths --output dist/mc263
)";

struct Options {
    std::string mcDir;
    std::string version = "26.3";
    std::string versionDir;
    std::string javaRuntime;
    std::string arch;
    std::string launcher = "build/launcher";
    std::string output = "dist/mc263";
    std::string workDir = "build/work";
    std::vector<std::string> modsDirs;
    std::vector<std::string> extraLibs;
    std::vector<std::string> extraJvmArgs;
    std::string username;
    bool allLibs = false;
    bool rpm = false;
    bool online = false;
    bool checkPaths = false;
    std::string mirror = "official";
    size_t jobs = 8;
    bool keepSignatures = false;
    bool nativesScan = true;
    bool verbose = false;
    bool help = false;
};

[[noreturn]] static void die(const std::string& msg) {
    std::println(stderr, "\n[错误] {}", msg);
    std::exit(1);
}
static bool fsxIsFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
// 直接 execv 调用工具，不经过 shell（路径含空格也安全）
static int runTool(const std::vector<std::string>& argv) {
    std::vector<std::string> a = argv;
    std::vector<char*> av;
    for (auto& s : a) av.push_back(s.data());
    av.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        ::execvp(a[0].c_str(), av.data());
        ::_exit(127);
    }
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void warn(const std::string& msg) { std::println(stderr, "[警告] {}", msg); }
static void step(const std::string& msg) { std::println("\n==> {}", msg); }

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}
static std::string toLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
static std::string nowIso() {
    std::time_t t = std::time(nullptr);
    std::tm lt{};
    localtime_r(&t, &lt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &lt);
    return buf;
}
static std::string detectArch() {
    struct utsname u{};
    if (uname(&u) == 0) {
        std::string m(u.machine);
        if (m == "aarch64" || m == "arm64") return "aarch64";
        return "x86_64";
    }
    return "x86_64";
}

static bool parseArgs(int argc, char** argv, Options& o, std::string& err) {
    o.mcDir = mc263::fsx::env("MC263_MC_DIR");
    if (o.mcDir.empty()) o.mcDir = mc263::fsx::home() + "/.minecraft";
    std::string v = mc263::fsx::env("MC263_VERSION");
    if (!v.empty()) o.version = v;
    o.arch = detectArch();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string key = arg, val;
        size_t eq = arg.find('=');
        if (eq != std::string::npos) { key = arg.substr(0, eq); val = arg.substr(eq + 1); }
        auto value = [&]() -> std::string {
            if (!val.empty()) return val;
            if (i + 1 < argc) return argv[++i];
            return {};
        };
        auto needValue = [&](const std::string& dest) -> bool {
            if (dest.empty()) { err = key + " 缺少参数值"; return false; }
            return true;
        };

        if (key == "--help" || key == "-h") { o.help = true; return true; }
        else if (key == "--mc-dir") { o.mcDir = value(); if (!needValue(o.mcDir)) return false; }
        else if (key == "--version") { o.version = value(); if (!needValue(o.version)) return false; }
        else if (key == "--version-dir") { o.versionDir = value(); if (!needValue(o.versionDir)) return false; }
        else if (key == "--java-runtime") { o.javaRuntime = value(); if (!needValue(o.javaRuntime)) return false; }
        else if (key == "--arch") { o.arch = value(); if (!needValue(o.arch)) return false; }
        else if (key == "--launcher") { o.launcher = value(); if (!needValue(o.launcher)) return false; }
        else if (key == "--output") { o.output = value(); if (!needValue(o.output)) return false; }
        else if (key == "--work-dir") { o.workDir = value(); if (!needValue(o.workDir)) return false; }
        else if (key == "--username") { o.username = value(); }
        else if (key == "--mods-dir") { std::string d = value(); if (!needValue(d)) return false; o.modsDirs.push_back(d); }
        else if (key == "--extra-lib") { std::string d = value(); if (!needValue(d)) return false; o.extraLibs.push_back(d); }
        else if (key == "--extra-jvm-arg" || key == "--jvm-arg") { std::string d = value(); if (!needValue(d)) return false; o.extraJvmArgs.push_back(d); }
        else if (key == "--all-libs") o.allLibs = true;
        else if (key == "--rpm") o.rpm = true;
        else if (key == "--online") o.online = true;
        else if (key == "--check-paths") o.checkPaths = true;
        else if (key == "--mirror") { o.mirror = value(); if (!needValue(o.mirror)) return false; }
        else if (key == "--jobs") { o.jobs = static_cast<size_t>(std::stoi(value())); }
        else if (key == "--keep-signatures") o.keepSignatures = true;
        else if (key == "--no-natives-scan") o.nativesScan = false;
        else if (key == "--preserve-work") { /* 兼容旧文档：默认即保留 */ }
        else if (key == "--verbose") o.verbose = true;
        else { err = "未知参数: " + arg; return false; }
    }
    err.clear();
    return true;
}

// --------------------------------------------------------------- JRE 探测 ----
static bool findRuntime(const Options& o, fs::path& out, std::string& err, std::vector<std::string>& notes) {
    if (!o.javaRuntime.empty()) {
        out = fs::path(o.javaRuntime);
        if (!fsxIsFile(out / "bin" / "java")) { err = "指定的 JRE 缺少 bin/java: " + out.string(); return false; }
        return true;
    }
    fs::path rtRoot = fs::path(o.mcDir) / "runtime";
    std::vector<fs::path> candidates;
    for (const fs::path& p : mc263::fsx::recursiveFiles(rtRoot)) {
        if (p.filename() == "java" && p.parent_path().filename() == "bin")
            candidates.push_back(p);
    }
    std::sort(candidates.begin(), candidates.end(), [](const fs::path& a, const fs::path& b) {
        auto score = [](const fs::path& p) {
            std::string s = p.generic_string();
            return (s.find("java-runtime-epsilon") != std::string::npos ? 0 : 1);
        };
        return score(a) < score(b);
    });
    if (!candidates.empty()) {
        out = candidates.front().parent_path().parent_path();
        return true;
    }
    std::string jh = mc263::fsx::env("JAVA_HOME");
    if (!jh.empty() && fsxIsFile(fs::path(jh) / "bin" / "java")) {
        out = fs::path(jh);
        notes.push_back("使用 JAVA_HOME: " + jh);
        return true;
    }
    // 最后兜底：系统 java（产物不再完全自包含，明确警告）
    FILE* pipe = popen("command -v java 2>/dev/null", "r");
    std::string line;
    if (pipe) {
        char buf[512];
        if (std::fgets(buf, sizeof(buf), pipe)) line = buf;
        pclose(pipe);
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (!line.empty()) {
        char resolved[PATH_MAX];
        if (::realpath(line.c_str(), resolved)) {
            fs::path javaBin(resolved);
            out = javaBin.parent_path().parent_path();
            notes.push_back("未找到 Mojang 运行时，改用系统 Java: " + std::string(resolved) +
                            "（成品将不含 JRE，换机可能无法启动）");
            return true;
        }
    }
    err = "找不到 Linux Java 运行时。请用 --java-runtime 指定含 bin/java 的 JRE 目录";
    return false;
}

// --------------------------------------------------------- 版本目录解析 ----
struct VersionInfo {
    fs::path dir;
    fs::path json;
    fs::path jar;
    std::string id;
    JValue meta;
};

static VersionInfo resolveVersion(const Options& o) {
    VersionInfo vi;
    vi.dir = o.versionDir.empty() ? fs::path(o.mcDir) / "versions" / o.version
                                  : fs::path(o.versionDir);
    if (!mc263::fsx::isDir(vi.dir)) die("版本目录不存在: " + vi.dir.string());
    std::string name = vi.dir.filename().string();
    vi.json = vi.dir / (name + ".json");
    if (!mc263::fsx::isFile(vi.json)) {
        auto js = mc263::fsx::listFiles(vi.dir, ".json");
        if (js.empty()) die("找不到版本 json: " + vi.dir.string());
        vi.json = js.front();
    }
    vi.jar = vi.dir / (name + ".jar");
    if (!mc263::fsx::isFile(vi.jar)) {
        auto jars = mc263::fsx::listFiles(vi.dir, ".jar");
        if (jars.empty()) die("找不到版本 jar: " + vi.dir.string());
        vi.jar = jars.front();
    }
    std::string err;
    std::string text = mc263::fsx::readText(vi.json, err);
    if (!err.empty()) die(err);
    vi.meta = mc263::jsonParse(text, err);
    if (!err.empty()) die("解析 " + vi.json.string() + " 失败: " + err);
    const JValue* id = vi.meta.find("id");
    vi.id = (id && id->isString() && !id->str().empty()) ? id->str() : o.version;
    return vi;
}

// ------------------------------------------------------------ JAR 签名剥离 ----
static bool repackVersionJar(const fs::path& srcJar, const fs::path& dstJar,
                             bool keepSignatures, size_t& kept, size_t& stripped, std::string& err) {
    mc263::zip::Archive ar;
    if (!ar.openFile(srcJar, err)) return false;
    mc263::zip::Writer w;
    if (!w.open(dstJar, err)) return false;
    for (const auto& e : ar.entries()) {
        const std::string& n = e.name;
        if (n.empty() || n.back() == '/') continue;
        std::string upper = toLower(n);
        // toLower 后再比较小写后缀
        bool sig = upper.rfind("meta-inf/", 0) == 0 &&
                   (endsWith(upper, ".sf") || endsWith(upper, ".dsa") ||
                    endsWith(upper, ".rsa") || endsWith(upper, ".ec"));
        if (sig && !keepSignatures) { ++stripped; continue; }
        std::vector<uint8_t> data;
        if (!ar.read(e, data, err)) return false;
        if (!w.addBytesDeflate(n, data, 0100644, err)) return false;
        ++kept;
    }
    return w.finish(err);
}

// --------------------------------------------------------- Maven 坐标转换 ----
static std::string mavenToPath(const std::string& name) {
    std::vector<std::string> parts = split(name, ':');
    if (parts.size() < 3) return {};
    std::string group = parts[0], artifact = parts[1], ver = parts[2];
    std::string classifier = parts.size() >= 4 ? parts[3] : "";
    std::string ext = "jar";
    size_t at = ver.find('@');
    if (at != std::string::npos) { ext = ver.substr(at + 1); ver = ver.substr(0, at); }
    std::string g;
    for (char c : group) g.push_back(c == '.' ? '/' : c);
    return g + "/" + artifact + "/" + ver + "/" + artifact + "-" + ver +
           (classifier.empty() ? "" : "-" + classifier) + "." + ext;
}

// version json 的 rules：按 allow/disallow 顺序评估，features 规则默认不生效
static bool libAllowed(const JValue& lib, const std::string& arch) {
    const JValue* rules = lib.find("rules");
    if (!rules || !rules->isArray() || rules->array().empty()) return true;
    bool allowed = false;
    for (const JValue& r : rules->array()) {
        bool match = true;
        const JValue* os = r.find("os");
        if (os && os->isObject()) {
            const JValue* n = os->find("name");
            std::string osname = n ? n->str() : "";
            if (!osname.empty() && osname != "linux" && osname != "unix" && osname != "unknown") match = false;
            const JValue* oa = os->find("arch");
            if (oa && oa->isString()) {
                std::string a = toLower(oa->str());
                if (arch == "aarch64" && a != "arm64" && a != "aarch64" && !a.empty()) match = false;
                if (arch == "x86_64" && (a == "arm64" || a == "aarch64" || a == "arm32")) match = false;
            }
        }
        if (r.find("features")) match = false;
        if (match) {
            const JValue* a = r.find("action");
            allowed = a ? a->str() == "allow" : true;
        }
    }
    return allowed;
}

struct CollectStats {
    size_t jars = 0, nativesJars = 0, soFiles = 0;
    std::vector<std::string> missing;
};

static bool extractNatives(const fs::path& jar, const fs::path& nativesDir,
                           std::set<std::string>& seenSo, CollectStats& stats,
                           const Options& o, std::string& err) {
    if (!mc263::fsx::makeDirs(nativesDir, err)) return false;
    mc263::zip::Archive ar;
    if (!ar.openFile(jar, err)) { warn("跳过无法解析的 natives jar: " + jar.string() + " (" + err + ")"); err.clear(); return true; }
    for (const auto& e : ar.entries()) {
        if (e.name.empty() || e.name.back() == '/') continue;
        std::string base = fs::path(e.name).filename().string();
        if (!(endsWith(base, ".so") || base.find(".so.") != std::string::npos)) continue;
        if (seenSo.count(base)) continue;
        std::vector<uint8_t> data;
        std::string e2;
        if (!ar.read(e, data, e2)) { warn("提取 " + base + " 失败: " + e2); continue; }
        fs::path dst = nativesDir / base;
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out) { err = "无法写 " + dst.string(); return false; }
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.close();
        ::chmod(dst.c_str(), 0755);
        seenSo.insert(base);
        ++stats.soFiles;
        if (o.verbose) std::cout << "    .so " << base << "\n";
    }
    err.clear();
    return true;
}

static bool collectLibraries(const Options& o, const VersionInfo& vi, const fs::path& libsDir,
                             const fs::path& appDir, const fs::path& nativesDir,
                             CollectStats& stats, std::string& err) {
    std::set<std::string> copied;
    std::set<std::string> seenSo;
    std::string e2;

    auto takeJar = [&](const fs::path& src, const std::string& display) -> bool {
        if (!mc263::fsx::isFile(src)) {
            stats.missing.push_back(display); // 缺库只警告，不中断构建
            return true;
        }
        std::string base = src.filename().string();
        if (copied.count(base)) return true;
        if (!mc263::fsx::copyFile(src, appDir / base, err)) return false;
        copied.insert(base);
        ++stats.jars;
        if (o.verbose) std::cout << "    jar " << base << "\n";
        return true;
    };

    if (o.allLibs) {
        for (const fs::path& p : mc263::fsx::recursiveFiles(libsDir, ".jar")) {
            std::string n = p.filename().string();
            if (n.find("natives-windows") != std::string::npos ||
                n.find("natives-macos") != std::string::npos ||
                n.find("natives-osx") != std::string::npos) continue;
            if (o.arch == "x86_64" && (n.find("arm64") != std::string::npos || n.find("arm32") != std::string::npos)) continue;
            if (o.arch == "aarch64" && (n.find("natives-linux") != std::string::npos &&
                                        n.find("arm64") == std::string::npos && n.find("arm32") == std::string::npos)) continue;
            if (!takeJar(p, p.generic_string())) return false;
        }
    } else {
        const JValue* libs = vi.meta.find("libraries");
        if (!libs || !libs->isArray()) die("版本 json 缺少 libraries 数组；可改用 --all-libs");
        for (const JValue& lib : libs->array()) {
            if (!libAllowed(lib, o.arch)) continue;
            // 主 artifact
            std::string path;
            const JValue* dl = lib.find("downloads");
            if (dl && dl->isObject()) {
                const JValue* art = dl->find("artifact");
                if (art && art->isObject()) {
                    const JValue* p = art->find("path");
                    if (p) path = p->str();
                }
            }
            std::string name;
            const JValue* nm = lib.find("name");
            if (nm) name = nm->str();
            if (path.empty()) path = mavenToPath(name);
            if (!path.empty()) {
                if (!takeJar(libsDir / path, path)) return false;
            }

            // natives classifier
            std::vector<std::string> candidates;
            if (o.arch == "aarch64") candidates = {"natives-linux-arm64", "natives-linux-aarch64", "natives-linux"};
            else candidates = {"natives-linux", "natives-linux-x86_64"};
            const JValue* natives = lib.find("natives");
            if (natives && natives->isObject()) {
                const JValue* ln = natives->find("linux");
                if (ln && !ln->str().empty()) {
                    std::string c = ln->str();
                    const std::string ph = "${arch}";
                    size_t pos;
                    while ((pos = c.find(ph)) != std::string::npos)
                        c.replace(pos, ph.size(), o.arch == "aarch64" ? "arm64" : "64");
                    candidates.insert(candidates.begin(), c);
                }
            }
            const JValue* classifiers = (dl && dl->isObject()) ? dl->find("classifiers") : nullptr;
            for (const std::string& cand : candidates) {
                std::string npath;
                if (classifiers && classifiers->isObject()) {
                    const JValue* c = classifiers->find(cand);
                    if (c && c->isObject()) {
                        const JValue* p = c->find("path");
                        if (p) npath = p->str();
                    }
                }
                if (npath.empty() && !name.empty()) npath = mavenToPath(name + ":" + cand);
                if (npath.empty()) continue;
                fs::path src = libsDir / npath;
                if (!mc263::fsx::isFile(src)) continue;
                if (!takeJar(src, npath)) return false;
                if (!extractNatives(src, nativesDir, seenSo, stats, o, err)) return false;
                ++stats.nativesJars;
                break; // 同一库只取与架构匹配的一个 classifier
            }
        }
    }

    // 补充扫描：覆盖 version json 未声明的 natives（jtracy / Fabric 附加库等）
    if (o.nativesScan) {
        for (const fs::path& p : mc263::fsx::recursiveFiles(libsDir, ".jar")) {
            std::string n = p.filename().string();
            if (n.find("natives-linux") == std::string::npos) continue;
            if (o.arch == "x86_64" && (n.find("arm64") != std::string::npos ||
                                       n.find("arm32") != std::string::npos ||
                                       n.find("aarch64") != std::string::npos)) continue;
            if (o.arch == "aarch64" && n.find("arm64") == std::string::npos &&
                n.find("aarch64") == std::string::npos && n.find("arm32") == std::string::npos) continue;
            if (!takeJar(p, p.generic_string())) return false;
            if (!extractNatives(p, nativesDir, seenSo, stats, o, err)) return false;
            ++stats.nativesJars;
        }
    }
    err.clear();
    return true;
}

// ------------------------------------------------------------- config.json ----
static std::string findAssetIndex(const JValue& meta, const fs::path& assetsDir) {
    const JValue* ai = meta.find("assetIndex");
    if (ai && ai->isObject()) {
        const JValue* id = ai->find("id");
        if (id && !id->str().empty()) return id->str();
    }
    auto idx = mc263::fsx::listFiles(assetsDir / "indexes", ".json");
    if (!idx.empty()) {
        std::string s = idx.back().stem().string();
        warn("版本 json 未声明 assetIndex.id，退化为 " + s);
        return s;
    }
    warn("找不到 assetIndex.id，使用版本号作为 assetIndex（可能缺资源）");
    return "";
}

static void pushUnique(JValue& arr, const std::string& s) {
    if (s.empty()) return;
    for (const JValue& v : arr.array()) if (v.isString() && v.str() == s) return;
    arr.push(JValue::stringV(s));
}

static std::string mainClassOf(const JValue& meta, bool& fabric) {
    std::string mc;
    const JValue* m = meta.find("mainClass");
    if (m) mc = m->str();
    const JValue* libs = meta.find("libraries");
    bool hasFabric = false;
    if (libs && libs->isArray()) {
        for (const JValue& l : libs->array()) {
            const JValue* n = l.find("name");
            if (n && n->str().find("fabric-loader") != std::string::npos) { hasFabric = true; break; }
        }
    }
    fabric = hasFabric || mc.find("fabricmc") != std::string::npos;
    if (mc.empty()) mc = fabric ? "net.fabricmc.loader.impl.launch.knot.KnotClient"
                                : "net.minecraft.client.main.Main";
    return mc;
}

static JValue buildConfig(const Options& o, const VersionInfo& vi, const fs::path& assetsDir) {
    JValue cfg = JValue::objectV();
    cfg.set("schema", JValue::numberV(1));
    cfg.set("version", JValue::stringV(vi.id));
    const JValue* type = vi.meta.find("type");
    cfg.set("versionType", JValue::stringV(type ? type->str() : "release"));
    cfg.set("assetIndex", JValue::stringV(findAssetIndex(vi.meta, assetsDir)));
    bool fabric = false;
    cfg.set("mainClass", JValue::stringV(mainClassOf(vi.meta, fabric)));
    cfg.set("fabric", JValue::booleanV(fabric));
    cfg.set("arch", JValue::stringV(o.arch));
    cfg.set("builtAt", JValue::stringV(nowIso()));
    if (!o.username.empty()) cfg.set("defaultUsername", JValue::stringV(o.username));

    // JVM 参数：对齐 Python 版（Linux 化），Java 25 特性保留
    JValue jvm = JValue::arrayV();
    for (const char* a : {"-Xms2G", "-Xmx4G", "-XX:+UseCompactObjectHeaders",
                          "-XX:+AlwaysPreTouch", "-XX:+UseStringDeduplication", "-XX:+UseZGC",
                          "--sun-misc-unsafe-memory-access=allow",
                          "--enable-native-access=ALL-UNNAMED"}) pushUnique(jvm, a);
    if (fabric) pushUnique(jvm, "-Dfabric.development=false");
    // version json 里不带占位符的静态 JVM 参数，去重合并（适配 26.3 新增项）
    const JValue* args = vi.meta.find("arguments");
    if (args && args->isObject()) {
        const JValue* ja = args->find("jvm");
        if (ja && ja->isArray()) {
            for (const JValue& a : ja->array()) {
                if (!a.isString()) continue;
                std::string s = a.str();
                if (s.empty() || s[0] != '-' || s.find("${") != std::string::npos) continue;
                if (s.find("-cp") == 0 || s.rfind("-Djava.library.path", 0) == 0 ||
                    s.rfind("-Djna.tmpdir", 0) == 0 ||
                    s.rfind("-Dorg.lwjgl.system.SharedLibraryExtractPath", 0) == 0 ||
                    s.rfind("-Dio.netty.native.workdir", 0) == 0) continue;
                pushUnique(jvm, s);
            }
        }
    }
    if (fabric) pushUnique(jvm, "-DFabricMcEmu= net.minecraft.client.main.Main ");
    for (const std::string& a : o.extraJvmArgs) pushUnique(jvm, a);
    cfg.set("jvmArgs", jvm);
    // 运行期动态参数（--username / --gameDir / --assetsDir ...）由启动器注入
    cfg.set("gameArgs", JValue::arrayV());
    return cfg;
}

// --------------------------------------------------------------- payload ----
static bool allowedInPayload(const std::string& rel) {
    for (const char* p : {"app/", "runtime/", "assets/", "mods/"})
        if (rel.rfind(p, 0) == 0) return true;
    return rel == "config.json" || rel == "version.json";
}

static bool createPayload(const fs::path& stage, const fs::path& zipPath, const Options& o,
                          size_t& files, std::string& err) {
    mc263::zip::Writer w;
    if (!w.open(zipPath, err)) return false;
    files = 0;
    for (const fs::path& p : mc263::fsx::recursiveFiles(stage)) {
        std::string rel = mc263::fsx::relativePosix(stage, p);
        if (!allowedInPayload(rel)) {
            if (o.verbose) std::cout << "    跳过 " << rel << "\n";
            continue;
        }
        uint32_t mode = mc263::fsx::unixMode(p);
        if (!w.addFile(rel, p, mode, err)) return false;
        ++files;
        if (o.verbose && files % 200 == 0) std::cout << "    " << files << " 个文件...\n";
    }
    return w.finish(err);
}

static bool assemble(const fs::path& launcher, const fs::path& payloadPath,
                     uint64_t payloadSize, uint32_t payloadCrc, uint32_t version,
                     const fs::path& output, uint64_t& finalSize, std::string& err) {
    { // 校验 launcher 是 ELF
        std::ifstream in(launcher, std::ios::binary);
        char magic[4] = {0, 0, 0, 0};
        if (!in || !in.read(magic, 4)) { err = "无法读取启动器 " + launcher.string(); return false; }
        if (!(magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F')) {
            err = "启动器不是 ELF 文件: " + launcher.string();
            return false;
        }
    }
    if (!mc263::fsx::copyFile(launcher, output, err)) return false;
    FILE* out = std::fopen(output.string().c_str(), "ab");
    FILE* zin = std::fopen(payloadPath.string().c_str(), "rb");
    if (!out || !zin) { err = "打开输出/payload 失败"; return false; }
    std::vector<uint8_t> buf(1 << 20);
    uint64_t left = payloadSize;
    while (left > 0) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
        size_t got = std::fread(buf.data(), 1, want, zin);
        if (got == 0 || std::fwrite(buf.data(), 1, got, out) != got) {
            err = "追加 payload 时写入失败";
            std::fclose(out); std::fclose(zin);
            return false;
        }
        left -= got;
    }
    std::string trailer = mc263::makeTrailer(payloadSize, payloadCrc, version);
    if (std::fwrite(trailer.data(), 1, trailer.size(), out) != trailer.size()) {
        err = "写入 trailer 失败";
        std::fclose(out); std::fclose(zin);
        return false;
    }
    std::fclose(out);
    std::fclose(zin);
    ::chmod(output.c_str(), 0755);
    finalSize = mc263::fsx::fileSize(output);
    err.clear();
    return true;
}

// ------------------------------------------------------------------- main ----
int main(int argc, char** argv) {
    Options opt;
    std::string err;
    if (!parseArgs(argc, argv, opt, err)) { std::cerr << "[错误] " << err << "\n\n" << kUsage; return 1; }
    if (opt.help) { std::print("{}", kUsage); return 0; }

    // 路径守卫：写入目标必须在沙箱内，且永不落到 /、/home、/etc 等敏感目录
    mc263::safe::Policy pol = mc263::safe::policyFromEnv(argv[0]);
    std::vector<std::pair<std::string, fs::path>> writes = {
        {"--output", fs::path(opt.output)},
        {"--work-dir", fs::path(opt.workDir)},
    };
    if (opt.rpm) writes.push_back({"rpm 输出目录", fs::path(opt.output).parent_path()});
    bool pathOk = true;
    for (const auto& [name, p] : writes) {
        auto r = mc263::safe::checkWrite(p, pol);
        std::println("  路径检查 {}: {} -> {}", name, p.string(), r.ok ? "允许" : r.reason);
        if (!r.ok) pathOk = false;
    }
    if (!pathOk) die("输出路径被安全策略拒绝（" + mc263::safe::describe(pol) + "）");
    if (opt.checkPaths) {
        std::println("路径检查通过（{}）", mc263::safe::describe(pol));
        return 0;
    }

    std::println("mc263-build | C++23 重写 build_262.py | Linux 单文件 Minecraft {}", opt.version);
    std::println("架构: {} | 来源: {}", opt.arch, opt.mcDir);

    if (opt.online) {
        step("在线下载游戏文件（mc263-fetch）");
        fs::path fetch = fs::absolute(argv[0]).parent_path() / "mc263-fetch";
        if (!mc263::fsx::isFile(fetch)) fetch = fs::path("mc263-fetch"); // 交给 PATH 查找
        std::vector<std::string> a = {
            fetch.string(), "--mc-dir", opt.mcDir, "--version", opt.version,
            "--mirror", opt.mirror, "--arch", opt.arch, "--jobs", std::to_string(opt.jobs)
        };
        int rc = runTool(a);
        if (rc != 0) die("在线下载失败 (rc=" + std::to_string(rc) +
                         ")；可先手动运行 mc263-fetch 排查，或换 --mirror bmclapi");
        std::println("  下载完成，继续构建");
    }

    fs::path mcDir(opt.mcDir);
    VersionInfo vi = resolveVersion(opt);
    fs::path libsDir = mcDir / "libraries";
    fs::path assetsDir = mcDir / "assets";
    std::cout << "版本目录: " << vi.dir << "\n版本 json: " << vi.json.filename()
              << " | id=" << vi.id << "\n版本 jar: " << vi.jar.filename() << "\n";

    std::vector<std::string> notes;
    fs::path runtime;
    if (!findRuntime(opt, runtime, err, notes)) die(err);
    for (const std::string& n : notes) warn(n);
    std::cout << "Java 运行时: " << runtime << "\n";
    if (mc263::fsx::isFile(runtime / "bin" / "java.exe") && !mc263::fsx::isFile(runtime / "bin" / "java"))
        die("检测到 Windows JRE（只有 java.exe）。Linux 单文件必须使用 Linux 版 JRE。");

    fs::path work(opt.workDir), stage = work / "stage", outDir = work / "out";
    fs::path appDir = stage / "app", nativesDir = appDir / "natives";
    fs::path rtDir = stage / "runtime", modsStage = stage / "mods";
    for (const fs::path& d : {work, outDir, stage, appDir, nativesDir, rtDir, modsStage}) {
        if (!mc263::fsx::makeDirs(d, err)) die(err);
    }
    fs::path outputParent = fs::path(opt.output).parent_path();
    if (!outputParent.empty() && !mc263::fsx::makeDirs(outputParent, err)) die(err);
    step("清理暂存目录");
    for (const fs::path& d : {appDir, nativesDir, rtDir, modsStage, stage / "assets"}) {
        if (!mc263::fsx::removeTree(d, err)) die(err);
        if (!mc263::fsx::makeDirs(d, err)) die(err);
    }

    // 1) JRE
    step("复制 Java 运行时（裁剪 man/src.zip/debuginfo/classes.jsa 等）");
    {
        auto keep = [](const std::string& rel, bool isDirFlag) -> bool {
            std::string name = fs::path(rel).filename().string();
            if (isDirFlag) return !(name == "man" || name == "demo" || name == "sample");
            if (name == "src.zip" || name == "classes.jsa") return false;
            for (const char* s : {".debuginfo", ".pack", ".pkg", ".pdb", ".diz",
                                  ".exe", ".dll", ".dylib"})
                if (endsWith(name, s)) return false;
            return true;
        };
        mc263::fsx::CopyStats st;
        if (!mc263::fsx::copyTree(runtime, rtDir, keep, st, err)) die(err);
        fs::path java = rtDir / "bin" / "java";
        if (!mc263::fsx::isFile(java)) die("复制后的 JRE 缺少 bin/java（目录结构异常）: " + rtDir.string());
        ::chmod(java.c_str(), 0755);
        std::cout << "  runtime: " << mc263::fsx::humanSize(mc263::fsx::dirSize(rtDir))
                  << " / " << st.files << " 个文件\n";
    }

    // 2) 版本 jar
    step("处理版本 jar（剥离 META-INF 签名）");
    {
        size_t kept = 0, stripped = 0;
        if (!repackVersionJar(vi.jar, appDir / "mc.jar", opt.keepSignatures, kept, stripped, err)) die(err);
        std::cout << "  app/mc.jar: " << kept << " 个条目，剥离签名 " << stripped << " 个，"
                  << mc263::fsx::humanSize(mc263::fsx::fileSize(appDir / "mc.jar")) << "\n";
    }

    // 3) libraries + natives
    step("收集 libraries 与 Linux natives");
    CollectStats st;
    if (!collectLibraries(opt, vi, libsDir, appDir, nativesDir, st, err)) die(err);
    for (const std::string& lib : opt.extraLibs) {
        fs::path p(lib);
        if (!mc263::fsx::isFile(p)) { warn("额外库不存在: " + lib); continue; }
        if (!mc263::fsx::copyFile(p, appDir / p.filename(), err)) die(err);
        ++st.jars;
    }
    std::cout << "  jar: " << st.jars << " | natives jar: " << st.nativesJars
              << " | .so: " << st.soFiles << "\n";
    if (!st.missing.empty()) {
        std::cout << "  缺失声明库 " << st.missing.size() << " 个（前 8 个）:\n";
        for (size_t i = 0; i < std::min<size_t>(8, st.missing.size()); ++i)
            std::cout << "    - " << st.missing[i] << "\n";
        warn("缺失的库可能导致启动失败；可用 --extra-lib 补，或改用 --all-libs");
    }
    if (st.jars < 5) warn("收集到的 jar 数量异常少，请检查 version json / --mc-dir");
    if (st.soFiles == 0) warn("没有提取到任何 .so，LWJGL 可能回退到 classpath 自动提取");

    // 4) mods
    step("复制 mods");
    {
        std::vector<fs::path> dirs = {vi.dir / "mods", mcDir / "mods"};
        for (const std::string& d : opt.modsDirs) dirs.emplace_back(d);
        size_t n = 0;
        for (const fs::path& d : dirs) {
            for (const fs::path& p : mc263::fsx::listFiles(d, ".jar")) {
                fs::path dst = modsStage / p.filename();
                if (mc263::fsx::exists(dst)) continue;
                if (!mc263::fsx::copyFile(p, dst, err)) die(err);
                ++n;
                if (opt.verbose) std::cout << "    mod " << p.filename().string() << "\n";
            }
        }
        std::cout << "  mods: " << n << " 个\n";
    }

    // 5) assets
    step("复制 assets");
    if (mc263::fsx::isDir(assetsDir)) {
        mc263::fsx::CopyStats st2;
        auto keepAll = [](const std::string&, bool) { return true; };
        if (!mc263::fsx::copyTree(assetsDir, stage / "assets", keepAll, st2, err)) die(err);
        std::cout << "  assets: " << mc263::fsx::humanSize(mc263::fsx::dirSize(stage / "assets"))
                  << " / " << st2.files << " 个文件\n";
    } else warn("assets 目录不存在，成品会缺少游戏资源: " + assetsDir.string());

    // 6) config + 原版 json 诊断信息
    step("生成 config.json");
    {
        JValue cfg = buildConfig(opt, vi, assetsDir);
        if (!mc263::fsx::writeText(stage / "config.json", mc263::jsonDump(cfg), err)) die(err);
        if (!mc263::fsx::copyFile(vi.json, stage / "version.json", err)) die(err);
        std::cout << "  mainClass=" << (cfg.find("mainClass") ? cfg.find("mainClass")->str() : "?")
                  << " | assetIndex=" << (cfg.find("assetIndex") ? cfg.find("assetIndex")->str() : "?")
                  << " | fabric=" << (cfg.find("fabric")->boolean() ? "yes" : "no") << "\n";
    }

    // 7) payload zip
    step("生成 game.zip（STORED）");
    fs::path zipPath = work / "game.zip";
    size_t payloadFiles = 0;
    if (!createPayload(stage, zipPath, opt, payloadFiles, err)) die(err);
    uint64_t zipSize = mc263::fsx::fileSize(zipPath);
    std::println("  {} 个文件，ZIP {}", payloadFiles, mc263::fsx::humanSize(zipSize));
    // CRC 始终针对「原始 game.zip」：v1 直接校验 payload，v2 校验解压结果
    FILE* zipFp = std::fopen(zipPath.string().c_str(), "rb");
    if (!zipFp) die("无法重新打开 game.zip");
    uint32_t zipCrc = mc263::zip::crc32Region(zipFp, 0, zipSize, err);
    std::fclose(zipFp);
    if (!err.empty()) die(err);

    fs::path payloadPath = zipPath;
    uint64_t payloadSize = zipSize;
    uint32_t fmtVersion = mc263::SFX_VERSION;
#ifdef MC263_WITH_ZSTD
    step("用系统 libzstd 压缩 payload（Fedora / openSUSE TW 自带）");
    fs::path zstPath = work / "game.zip.zst";
    uint64_t zstSize = 0;
    if (!mc263::zstd::compressFile(zipPath, zstPath, zstSize, err)) die(err);
    std::println("  zstd: {} -> {}（{:.1f}% 体积）", mc263::fsx::humanSize(zipSize),
                 mc263::fsx::humanSize(zstSize),
                 zipSize ? 100.0 * static_cast<double>(zstSize) / static_cast<double>(zipSize) : 0.0);
    payloadPath = zstPath;
    payloadSize = zstSize;
    fmtVersion = mc263::SFX_VERSION_ZSTD;
#endif

    // 8) 拼装单文件
    step("拼装单文件: [launcher ELF] + [payload] + [trailer]");
    fs::path launcher(opt.launcher);
    if (!mc263::fsx::isFile(launcher)) die("找不到启动器 ELF: " + launcher.string() + "（先执行 make，或用 --launcher 指定）");
    fs::path output(opt.output);
    uint64_t finalSize = 0;
    if (!assemble(launcher, payloadPath, payloadSize, zipCrc, fmtVersion, output, finalSize, err)) die(err);
    std::println("  输出: {} ({}) | payload 格式 v{}", fs::absolute(output).string(),
                 mc263::fsx::humanSize(finalSize), fmtVersion);
    // 分发完整性：调用系统 coreutils 的 sha256sum（Fedora / openSUSE TW 自带）
    {
        std::string p = fs::absolute(output).string(), esc;
        for (char c : p) { if (c == '\'') esc += "'\\''"; else esc += c; }
        FILE* pipe = popen(("sha256sum '" + esc + "' 2>/dev/null").c_str(), "r");
        if (pipe) {
            char buf[256];
            if (std::fgets(buf, sizeof(buf), pipe)) {
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                if (line.size() >= 64) std::println("  SHA256: {}", line.substr(0, 64));
            }
            pclose(pipe);
        }
    }

    // 9) 可选 rpm
    if (opt.rpm) {
        step("生成 rpm");
        fs::path script = fs::absolute(argv[0]).parent_path().parent_path() / "packaging" / "build-rpm.sh";
        if (!mc263::fsx::isFile(script)) script = fs::path("packaging/build-rpm.sh");
        if (!mc263::fsx::isFile(script)) die("找不到 packaging/build-rpm.sh");
        std::string cmd = "bash " + script.string() + " --payload " + fs::absolute(output).string();
        int rc = std::system(cmd.c_str());
        if (rc != 0) die("rpm 打包失败 (rc=" + std::to_string(rc) + ")");
    }

    step("完成");
    const std::string exe = fs::absolute(output).string();
    std::println("运行方式:\n  {}                 # 便携模式（同目录存档）\n  {} --username Steve\n  MC263_USERNAME=Alex {}\n"
                 "打包成 rpm（安装后菜单/命令一键启动）:\n  bash packaging/build-rpm.sh --payload {}\n"
                 "诊断: 加 --extract-only 只解压不启动；日志在 gameDir/launcher.log",
                 exe, exe, exe, exe);
    return 0;
}
