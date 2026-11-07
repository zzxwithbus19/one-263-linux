// launcher.cpp -- Minecraft 26.3 Linux 单文件启动器（C++17，零第三方依赖）
//
// 单文件布局: [本程序 ELF][game.zip (STORED)][32 字节 trailer]
// 首次运行: 从 /proc/self/exe 定位自身 -> 校验 payload CRC -> 解压到数据目录
//           -> 安装 mods -> 修可执行位 -> 拼 classpath -> fork/exec 内嵌 Java。
//
// 两种运行形态:
//   * 便携模式（exe 所在目录可写）: _data/ 与存档都在 exe 同级，双击即玩
//   * 系统模式（rpm 装到 /opt 等只读目录）: 解压到 $XDG_CACHE_HOME/mc263，
//     存档到 $XDG_DATA_HOME/mc263，配合 .desktop 一键启动
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
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static const char* kUsage = R"(mc263 -- Minecraft 26.3 Linux 单文件启动器

用法: mc263 [选项] [-- 传给游戏的额外参数]

  -u, --username NAME    玩家名（默认: 可执行文件名 / $USER）
      --game-dir PATH    存档与 mods 目录（默认见下方"目录规则"）
      --data-dir PATH    解压数据目录（默认见下方"目录规则"）
      --java PATH        用指定 java 启动（调试用，默认内嵌 JRE）
      --jvm-arg ARG      追加 JVM 参数（可重复）
      --extract-only     只解压/安装，不启动游戏
      --force-extract    忽略缓存，强制重新解压
      --no-verify        跳过 payload CRC32 校验
      --info             打印单文件与路径信息后退出
  -v, --verbose          详细输出
  -h, --help             显示本帮助

环境变量:
  MC263_USERNAME, MC263_GAME_DIR, MC263_DATA_DIR, MC263_JAVA,
  MC263_PORTABLE=1（强制便携）, MC263_NO_PORTABLE=1（强制系统目录）,
  MC263_SANDBOX_ROOT, MC263_ALLOW_OUTSIDE=1, XDG_CACHE_HOME, XDG_DATA_HOME

目录规则（全部经过路径守卫）:
  exe 所在目录可写 -> 便携: <exe目录>/_data + <exe目录>/saves|mods
  否则（rpm 安装） -> 先尝试 XDG 目录，被策略拒绝时自动回退
                     <沙箱>/runtime-data 或 /tmp/mc263-<uid>（重启后不保留，会打印提示）
  日志: <gameDir>/launcher.log；启动失败: <gameDir>/launcher_error.log

路径安全: 默认只写项目/可执行文件所在的沙箱目录；
  /、/home、/etc、/usr 等系统敏感路径一律拒绝（显式 --game-dir/--data-dir 也一样）。
  需要持久化数据时，用 MC263_SANDBOX_ROOT=<专用目录> 并把数据目录放进去。
)";

struct Options {
    std::string username, gameDir, dataDir, java;
    std::vector<std::string> jvmArgs;
    std::vector<std::string> passthrough;
    bool extractOnly = false, forceExtract = false, noVerify = false;
    bool info = false, verbose = false, help = false;
};

struct Paths {
    fs::path exe, exeDir, dataDir, gameDir, sandbox;
    std::string exeStem;
    std::string error;
    bool portable = false;
};

static std::string timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm lt{};
    localtime_r(&t, &lt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    return buf;
}
static void logLine(const fs::path& logPath, const std::string& line) {
    std::ofstream o(logPath, std::ios::app);
    if (o) o << "[" << timestamp() << "] " << line << "\n";
}
static int fail(const fs::path& gameDir, const fs::path& exeDir, const std::string& msg) {
    std::println(stderr, "错误: {}", msg);
    fs::path base = gameDir.empty() ? exeDir : gameDir;
    fs::path logTarget = base / "launcher_error.log";
    std::error_code ec;
    if (mc263::safe::checkWrite(logTarget, mc263::safe::policyFromEnv()).ok) {
        fs::create_directories(base, ec);
        if (!ec) {
            std::ofstream o(logTarget, std::ios::trunc);
            if (o) o << timestamp() << "\n" << msg << "\n";
        }
    } else {
        std::println(stderr, "（已跳过错误日志写入：目标路径被安全策略拒绝）");
    }
    return 1;
}

static bool parseArgs(int argc, char** argv, Options& o, std::string& err) {
    bool passthroughMode = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (passthroughMode) { o.passthrough.push_back(arg); continue; }
        if (arg == "--") { passthroughMode = true; continue; }
        std::string key = arg, val;
        size_t eq = arg.find('=');
        if (eq != std::string::npos) { key = arg.substr(0, eq); val = arg.substr(eq + 1); }
        auto value = [&]() -> std::string {
            if (!val.empty()) return val;
            if (i + 1 < argc) return argv[++i];
            return {};
        };
        auto need = [&](const std::string& v) -> bool {
            if (v.empty()) { err = key + " 缺少参数值"; return false; }
            return true;
        };
        if (key == "--help" || key == "-h") { o.help = true; return true; }
        else if (key == "--username" || key == "-u") { o.username = value(); if (!need(o.username)) return false; }
        else if (key == "--game-dir") { o.gameDir = value(); if (!need(o.gameDir)) return false; }
        else if (key == "--data-dir") { o.dataDir = value(); if (!need(o.dataDir)) return false; }
        else if (key == "--java") { o.java = value(); if (!need(o.java)) return false; }
        else if (key == "--jvm-arg") { std::string a = value(); if (!need(a)) return false; o.jvmArgs.push_back(a); }
        else if (key == "--extract-only") o.extractOnly = true;
        else if (key == "--force-extract") o.forceExtract = true;
        else if (key == "--no-verify") o.noVerify = true;
        else if (key == "--info") o.info = true;
        else if (key == "--verbose" || key == "-v") o.verbose = true;
        else o.passthrough.push_back(arg); // 未知参数直接透传给 Minecraft
    }
    err.clear();
    return true;
}

static fs::path selfExe(const char* argv0) {
    char buf[PATH_MAX + 1];
    ssize_t n = ::readlink("/proc/self/exe", buf, PATH_MAX);
    if (n > 0) { buf[n] = '\0'; return fs::path(buf); }
    if (argv0) {
        char resolved[PATH_MAX + 1];
        if (::realpath(argv0, resolved)) return fs::path(resolved);
    }
    return fs::path(argv0 ? argv0 : "mc263");
}

static fs::path xdgDir(const std::string& envName, const std::string& fallback) {
    std::string v = mc263::fsx::env(envName);
    if (!v.empty()) return fs::path(v) / "mc263";
    return fs::path(mc263::fsx::home()) / fallback / "mc263";
}

static Paths computePaths(const fs::path& exe, const Options& o) {
    Paths p;
    p.exe = exe;
    p.exeDir = exe.parent_path();
    if (p.exeDir.empty()) p.exeDir = fs::current_path();
    p.exeStem = exe.stem().string();
    bool forcePortable = mc263::fsx::env("MC263_PORTABLE") == "1";
    bool forceSystem = mc263::fsx::env("MC263_NO_PORTABLE") == "1";
    bool exeWritable = mc263::fsx::isWritableDir(p.exeDir);
    p.portable = forcePortable || (!forceSystem && exeWritable);
    if (p.portable && !exeWritable) {
        std::cerr << "警告: 指定便携模式但 " << p.exeDir << " 不可写，回退到系统目录\n";
        p.portable = false;
    }
    std::string envData = mc263::fsx::env("MC263_DATA_DIR");
    std::string envGame = mc263::fsx::env("MC263_GAME_DIR");
    bool explicitData = !o.dataDir.empty() || !envData.empty();
    bool explicitGame = !o.gameDir.empty() || !envGame.empty();

    if (!o.dataDir.empty()) p.dataDir = fs::absolute(o.dataDir);
    else if (!envData.empty()) p.dataDir = fs::absolute(envData);
    else if (p.portable) p.dataDir = p.exeDir / "_data";
    else p.dataDir = xdgDir("XDG_CACHE_HOME", ".cache") / "_data";

    if (!o.gameDir.empty()) p.gameDir = fs::absolute(o.gameDir);
    else if (!envGame.empty()) p.gameDir = fs::absolute(envGame);
    else if (p.portable) p.gameDir = p.exeDir;
    else p.gameDir = xdgDir("XDG_DATA_HOME", ".local/share");

    // 运行时写入守卫：显式指定的路径被拒就报错；自动路径逐级回退到安全目录
    mc263::safe::Policy pol;
    std::string envRoot = mc263::fsx::env("MC263_SANDBOX_ROOT");
    if (!envRoot.empty()) {
        pol.sandboxRoot = mc263::safe::normalize(fs::path(envRoot));
    } else {
        fs::path proj = mc263::safe::defaultSandboxRoot(exe.string().c_str());
        fs::path exeDirAbs = mc263::safe::normalize(p.exeDir);
        pol.sandboxRoot = mc263::safe::isUnder(exeDirAbs, proj) ? proj : exeDirAbs;
    }
    pol.allowOutside = mc263::fsx::env("MC263_ALLOW_OUTSIDE") == "1";
    p.sandbox = pol.sandboxRoot;

    auto apply = [&](fs::path& target, const char* sub, bool explicitPath, const char* what) {
        auto r = mc263::safe::checkWrite(target, pol);
        if (r.ok) return;
        if (explicitPath) { p.error = std::string(what) + ": " + r.reason; return; }
        fs::path fallback = pol.sandboxRoot / "runtime-data" / sub;
        auto r2 = mc263::safe::checkWrite(fallback, pol);
        if (r2.ok && mc263::fsx::isWritableDir(pol.sandboxRoot)) {
            std::println(stderr, "警告: {} 被路径策略拒绝，改用 {}", r.reason, fallback.string());
            target = fallback;
            return;
        }
        fs::path tmp = fs::path("/tmp") /
                       ("mc263-" + std::to_string(static_cast<unsigned>(::getuid()))) / sub;
        std::println(stderr, "警告: {} 被路径策略拒绝，改用临时目录 {}（重启后可能丢失）",
                     r.reason, tmp.string());
        target = tmp;
    };
    apply(p.dataDir, "data", explicitData, "--data-dir / MC263_DATA_DIR");
    apply(p.gameDir, "game", explicitGame, "--game-dir / MC263_GAME_DIR");
    return p;
}

// 找内嵌 java：dataDir/runtime/bin/java，或 runtime/ 下任意 */bin/java
static fs::path findJava(const fs::path& dataDir, const Options& o, std::string& note) {
    std::string fromEnv = o.java.empty() ? mc263::fsx::env("MC263_JAVA") : o.java;
    if (!fromEnv.empty()) {
        fs::path p(fromEnv);
        if (mc263::fsx::isFile(p)) { note = "外部指定 java"; return fs::absolute(p); }
        note = "指定的 java 不存在，回退";
    }
    fs::path direct = dataDir / "runtime" / "bin" / "java";
    if (mc263::fsx::isFile(direct)) { note = "内嵌 Java 运行时"; return direct; }
    for (const fs::path& p : mc263::fsx::recursiveFiles(dataDir / "runtime")) {
        if (p.filename() == "java" && p.parent_path().filename() == "bin") {
            note = "内嵌 Java 运行时 (" + p.parent_path().parent_path().filename().string() + ")";
            return p;
        }
    }
    FILE* pipe = popen("command -v java 2>/dev/null", "r");
    std::string line;
    if (pipe) {
        char buf[512];
        if (std::fgets(buf, sizeof(buf), pipe)) line = buf;
        pclose(pipe);
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (!line.empty() && mc263::fsx::isFile(fs::path(line))) {
        note = "系统 java（单文件内未找到 JRE）";
        return fs::path(line);
    }
    note = "未找到 java";
    return {};
}

static void fixExecutables(const fs::path& dataDir) {
    fs::path bin = dataDir / "runtime" / "bin";
    for (const fs::path& p : mc263::fsx::listFiles(bin))
        ::chmod(p.c_str(), 0755);
    for (const char* name : {"jspawnhelper", "jexec"})
        for (const fs::path& p : mc263::fsx::recursiveFiles(dataDir / "runtime"))
            if (p.filename() == name) ::chmod(p.c_str(), 0755);
    for (const fs::path& p : mc263::fsx::listFiles(dataDir / "app" / "natives"))
        if (p.filename().string().find(".so") != std::string::npos) ::chmod(p.c_str(), 0755);
}

static size_t installMods(const fs::path& dataDir, const fs::path& gameDir, bool verbose) {
    fs::path src = dataDir / "mods";
    fs::path dst = gameDir / "mods";
    auto mods = mc263::fsx::listFiles(src, ".jar");
    if (mods.empty()) return 0;
    std::error_code ec;
    fs::create_directories(dst, ec);
    size_t n = 0;
    for (const fs::path& p : mods) {
        fs::path target = dst / p.filename();
        if (mc263::fsx::exists(target)) continue; // 不覆盖玩家自己的 mod
        std::string err;
        if (mc263::fsx::copyFile(p, target, err)) {
            ++n;
            if (verbose) std::println("  安装 mod: {}", p.filename().string());
        } else std::println(stderr, "警告: 安装 mod 失败 {}", err);
    }
    return n;
}

// payload 视图：v1 直接指向自身 ELF 尾部；v2 指向 libzstd 解压出的临时 game.zip
struct PayloadView {
    FILE* f = nullptr;
    uint64_t off = 0, size = 0;
    fs::path tempZip; // 用完删除
};

static std::string cacheMark(const mc263::SfxTrailer& tr) {
    return "v" + std::to_string(tr.version) + "|" +
           std::to_string(tr.payloadSize) + "|" + std::to_string(tr.payloadCrc);
}
static bool cacheHit(const fs::path& dataDir, const std::string& mark) {
    fs::path ready = dataDir / ".mc263-ready";
    if (!mc263::fsx::isFile(ready)) return false;
    std::string err;
    std::string old = mc263::fsx::readText(ready, err);
    return err.empty() && old == mark;
}

// 返回 0 成功；1 失败（message 里带原因）
static int ensureExtracted(const PayloadView& view, uint32_t expectedCrc,
                           const std::string& mark, const Paths& paths,
                           const Options& o, bool verifyCrc, std::string& message) {
    std::string err;
    if (verifyCrc) {
        uint32_t crc = mc263::zip::crc32Region(view.f, view.off, view.size, err);
        if (!err.empty()) { message = "payload CRC 校验读取失败: " + err; return 1; }
        if (crc != expectedCrc) {
            message = "payload CRC32 不匹配（文件可能被截断/损坏），拒绝解压";
            return 1;
        }
    }

    fs::path tmp = paths.dataDir.parent_path() /
                   (paths.dataDir.filename().string() + ".tmp-" + std::to_string(::getpid()));
    if (!mc263::fsx::removeTree(tmp, err) || !mc263::fsx::makeDirs(tmp, err)) {
        message = "创建临时解压目录失败: " + err; return 1;
    }
    std::println("首次运行：正在解压游戏文件到 {} ...", paths.dataDir.string());
    mc263::zip::Archive ar;
    if (!ar.openRegion(view.f, view.off, view.size, err)) {
        message = "读取内嵌 ZIP 失败: " + err; return 1;
    }
    size_t lastPct = 101;
    bool ok = ar.extractAll(tmp, [&](size_t done, size_t total, const std::string& name) {
        if (o.verbose) std::println("  {}/{} {}", done, total, name);
        else if (total) {
            size_t pct = done * 100 / total / 5 * 5;
            if (pct != lastPct && pct % 10 == 0) {
                std::print("  {}% ({}/{})\r", pct, done, total);
                std::fflush(stdout);
                lastPct = pct;
            }
        }
    }, err);
    std::println("");
    if (!ok) { message = "解压失败: " + err; mc263::fsx::removeTree(tmp, err); return 1; }

    fixExecutables(tmp);
    // mods 在数据目录替换前就装到 gameDir，避免重复安装
    fs::path modStage = tmp / "mods";
    if (mc263::fsx::isDir(modStage)) {
        fs::path modsDest = paths.gameDir / "mods";
        std::error_code ec;
        fs::create_directories(modsDest, ec);
        for (const fs::path& p : mc263::fsx::listFiles(modStage, ".jar")) {
            fs::path dst = modsDest / p.filename();
            if (!mc263::fsx::exists(dst)) {
                std::string e2;
                if (!mc263::fsx::copyFile(p, dst, e2)) std::println(stderr, "警告: {}", e2);
            }
        }
    }
    if (!mc263::fsx::writeText(tmp / ".mc263-ready", mark, err)) {
        message = "写缓存标记失败: " + err; return 1;
    }
    if (!mc263::fsx::removeTree(paths.dataDir, err)) {
        message = "清理旧数据目录失败: " + err; return 1;
    }
    std::error_code ec;
    fs::rename(tmp, paths.dataDir, ec);
    if (ec) {
        // 并发启动时另一进程可能已经完成，直接使用已有目录
        if (mc263::fsx::isDir(paths.dataDir)) {
            mc263::fsx::removeTree(tmp, err);
            message = "另一实例已完成解压，复用其数据目录";
            return 0;
        }
        message = "重命名解压目录失败: " + ec.message();
        return 1;
    }
    message = "解压完成 (" + std::to_string(ar.entries().size()) + " 个条目)";
    return 0;
}

static std::string resolveUsername(const Options& o, const Paths& p, const mc263::JValue& cfg) {
    if (!o.username.empty()) return o.username;
    std::string envUser = mc263::fsx::env("MC263_USERNAME");
    if (!envUser.empty()) return envUser;
    const mc263::JValue* def = cfg.find("defaultUsername");
    if (def && !def->str().empty()) return def->str();
    if (!p.exeStem.empty() && p.exeStem != "mc263" && p.exeStem != "mc263-build" &&
        p.exeStem != "launcher" && p.exeStem != "mc263-launcher")
        return p.exeStem; // 重命名 exe 即改名（沿用 Windows 版行为）
    std::string user = mc263::fsx::env("USER");
    return user.empty() ? std::string("Player") : user;
}

int main(int argc, char** argv) {
    Options opt;
    std::string err;
    if (!parseArgs(argc, argv, opt, err)) return fail({}, fs::current_path(), err);
    if (opt.help) { std::print("{}", kUsage); return 0; }

    Paths paths = computePaths(selfExe(argv[0]), opt);
    if (!paths.error.empty()) return fail({}, paths.exeDir, paths.error);
    fs::path logPath = paths.gameDir / "launcher.log";
    std::error_code ec;
    fs::create_directories(paths.gameDir, ec);
    for (const char* sub : {"saves", "mods", "resourcepacks", "screenshots"})
        fs::create_directories(paths.gameDir / sub, ec);

    FILE* self = std::fopen(paths.exe.string().c_str(), "rb");
    if (!self) return fail(paths.gameDir, paths.exeDir, "无法打开自身: " + paths.exe.string());
    if (fseeko(self, 0, SEEK_END) != 0) { std::fclose(self); return fail(paths.gameDir, paths.exeDir, "无法定位文件末尾"); }
    off_t fileSizeOff = ftello(self);
    uint64_t fileSize = fileSizeOff > 0 ? static_cast<uint64_t>(fileSizeOff) : 0;

    mc263::SfxTrailer tr;
    bool hasPayload = false;
    if (!mc263::readTrailer(self, fileSize, tr, hasPayload, err)) {
        std::fclose(self);
        return fail(paths.gameDir, paths.exeDir, "读取单文件尾部信息失败: " + err);
    }
    uint64_t payloadOff = hasPayload ? fileSize - mc263::SFX_TRAILER_SIZE - tr.payloadSize : 0;

    if (opt.info) {
        std::println("mc263 单文件信息\n  exe          : {}\n  文件大小     : {}\n  内嵌 payload : {}",
                     paths.exe.string(), mc263::fsx::humanSize(fileSize),
                     hasPayload ? "是" : "否（裸启动器/外部数据模式）");
        if (hasPayload)
            std::println("  payload 大小 : {} | crc32={} | 格式=v{} ({})",
                         mc263::fsx::humanSize(tr.payloadSize), tr.payloadCrc, tr.version,
                         tr.version == mc263::SFX_VERSION_ZSTD ? "zstd" : "STORED zip");
        std::println("  运行模式     : {}\n  沙箱根       : {}\n  数据目录     : {}\n  游戏目录     : {}",
                     paths.portable ? "便携" : "系统目录", paths.sandbox.string(),
                     paths.dataDir.string(), paths.gameDir.string());
        std::fclose(self);
        return 0;
    }

    logLine(logPath, "=== 启动 === exe=" + paths.exe.string() +
                     " payload=" + (hasPayload ? std::to_string(tr.payloadSize) : "none") +
                     " mode=" + (paths.portable ? "portable" : "system"));

    // 1) 自解压：缓存命中时完全不触碰 payload，二次启动秒开
    const std::string mark = cacheMark(tr);
    if (hasPayload) {
        if (!opt.forceExtract && cacheHit(paths.dataDir, mark)) {
            std::println("  缓存命中，跳过解压（{}）", paths.dataDir.string());
            logLine(logPath, "缓存命中，跳过解压");
        } else {
            PayloadView view;
            bool verifyCrc = false;
            if (tr.version == mc263::SFX_VERSION) {
                view = {self, payloadOff, tr.payloadSize, {}};
                verifyCrc = !opt.noVerify;
            } else {
#ifdef MC263_WITH_ZSTD
                std::error_code ec2;
                fs::create_directories(paths.dataDir.parent_path(), ec2);
                fs::path tempZip = paths.dataDir.parent_path() /
                                   ("game.zip.tmp-" + std::to_string(::getpid()));
                FILE* tmpF = std::fopen(tempZip.string().c_str(), "wb+");
                if (!tmpF) {
                    std::fclose(self);
                    return fail(paths.gameDir, paths.exeDir, "无法创建临时解压文件: " + tempZip.string());
                }
                uint64_t origSize = 0;
                if (!mc263::zstd::decompressRegion(self, payloadOff, tr.payloadSize,
                                                   tmpF, origSize, err)) {
                    std::fclose(tmpF); std::fclose(self);
                    fs::remove(tempZip, ec2);
                    return fail(paths.gameDir, paths.exeDir, "zstd 解压 payload 失败: " + err);
                }
                uint32_t crc = mc263::zip::crc32Region(tmpF, 0, origSize, err);
                if (!err.empty() || crc != tr.payloadCrc) {
                    std::fclose(tmpF); std::fclose(self);
                    fs::remove(tempZip, ec2);
                    return fail(paths.gameDir, paths.exeDir,
                                "解压后 game.zip CRC32 不匹配，拒绝继续（payload 已损坏）");
                }
                view = {tmpF, 0, origSize, tempZip};
                std::println("  libzstd 解压 payload: {} -> {}",
                             mc263::fsx::humanSize(tr.payloadSize), mc263::fsx::humanSize(origSize));
#else
                std::fclose(self);
                return fail(paths.gameDir, paths.exeDir,
                            "该单文件使用 zstd payload (v2)，但本启动器未链接 libzstd；"
                            "请用 USE_ZSTD=1 重新 make，或重新构建为 v1 格式");
#endif
            }
            std::string msg;
            int rc = ensureExtracted(view, tr.payloadCrc, mark, paths, opt, verifyCrc, msg);
            if (!view.tempZip.empty()) {
                std::fclose(view.f);
                std::error_code ec2;
                fs::remove(view.tempZip, ec2);
            }
            if (rc == 1) {
                std::fclose(self);
                return fail(paths.gameDir, paths.exeDir, msg);
            }
            std::println("  {}", msg);
            logLine(logPath, msg);
        }
    } else {
        std::println("  未检测到内嵌 payload，使用外部数据目录 {}", paths.dataDir.string());
        if (!mc263::fsx::isDir(paths.dataDir))
            return fail(paths.gameDir, paths.exeDir,
                        "数据目录不存在: " + paths.dataDir.string() +
                        "（裸启动器请用 --data-dir 指定解压后的目录）");
    }
    std::fclose(self);

    // 2) 读配置
    fs::path cfgPath = paths.dataDir / "config.json";
    std::string cfgText = mc263::fsx::readText(cfgPath, err);
    mc263::JValue cfg = mc263::jsonParse(cfgText, err);
    if (!err.empty()) return fail(paths.gameDir, paths.exeDir, "解析 config.json 失败: " + err + " (" + cfgPath.string() + ")");
    auto cs = [&](const char* key, const std::string& def) -> std::string {
        const mc263::JValue* v = cfg.find(key);
        return (v && v->isString()) ? v->str() : def;
    };
    std::string version = cs("version", "26.3");
    std::string assetIndex = cs("assetIndex", version);
    std::string versionType = cs("versionType", "release");
    std::string mainClass = cs("mainClass", "net.minecraft.client.main.Main");
    bool fabric = cfg.find("fabric") && cfg.find("fabric")->boolean();

    // 3) 找 java
    std::string javaNote;
    fs::path java = findJava(paths.dataDir, opt, javaNote);
    if (java.empty()) return fail(paths.gameDir, paths.exeDir,
                                  "找不到 Java：内嵌 JRE 与系统 PATH 中都没有 java。可用 --java 指定");
    std::println("  Java: {} ({})", java.string(), javaNote);

    // 4) classpath（Linux 用冒号）
    std::vector<fs::path> jars = mc263::fsx::listFiles(paths.dataDir / "app", ".jar");
    if (jars.empty()) return fail(paths.gameDir, paths.exeDir,
                                  "app/ 下没有 jar，数据目录可能不完整: " + paths.dataDir.string());
    std::string cp;
    for (const fs::path& p : jars) {
        if (!cp.empty()) cp += ':';
        cp += p.string();
    }
    fs::path nativesDir = paths.dataDir / "app" / "natives";
    fs::path assetsDir = paths.dataDir / "assets";
    fs::create_directories(nativesDir, ec);
    installMods(paths.dataDir, paths.gameDir, opt.verbose);

    // 5) 组装命令行
    std::vector<std::string> args;
    args.push_back(java.string()); // execv 的 argv[0]
    const mc263::JValue* jvm = cfg.find("jvmArgs");
    if (jvm && jvm->isArray())
        for (const mc263::JValue& a : jvm->array()) if (a.isString()) args.push_back(a.str());
    args.push_back("-Djava.library.path=" + nativesDir.string());
    args.push_back("-Djna.tmpdir=" + nativesDir.string());
    args.push_back("-Dorg.lwjgl.system.SharedLibraryExtractPath=" + nativesDir.string());
    args.push_back("-Dio.netty.native.workdir=" + nativesDir.string());
    for (const std::string& a : opt.jvmArgs) args.push_back(a);
    args.push_back("-cp");
    args.push_back(cp);
    args.push_back(mainClass);

    std::string username = resolveUsername(opt, paths, cfg);
    std::vector<std::string> game = {
        "--username", username,
        "--version", version,
        "--gameDir", paths.gameDir.string(),
        "--assetsDir", assetsDir.string(),
        "--assetIndex", assetIndex,
        "--uuid", "00000000-0000-0000-0000-000000000000",
        "--accessToken", "0",
        "--clientId", "00000000-0000-0000-0000-000000000000",
        "--xuid", "0000000000000000",
        "--userType", "mojang",
        "--versionType", versionType,
    };
    (void)fabric; // 预留：Fabric 特定参数已由 config.jvmArgs 注入
    for (const std::string& g : game) args.push_back(g);
    const mc263::JValue* extraGame = cfg.find("gameArgs");
    if (extraGame && extraGame->isArray())
        for (const mc263::JValue& a : extraGame->array()) if (a.isString()) args.push_back(a.str());
    for (const std::string& a : opt.passthrough) args.push_back(a);

    std::string cmdForLog = mc263::fsx::joinForLog(args);
    logLine(logPath, "username=" + username + " gameDir=" + paths.gameDir.string());
    logLine(logPath, "cmd=" + cmdForLog);
    if (opt.verbose) std::println("  cmd: {}", cmdForLog);
    if (opt.extractOnly) {
        std::println("  --extract-only：解压完成，未启动游戏。\n  config: {}\n  jars: {}",
                     cfgPath.string(), jars.size());
        return 0;
    }

    std::println("正在启动 Minecraft {}（玩家 {}）...\n日志: {}",
                 version, username, logPath.string());

    // 6) fork + exec
    std::vector<char*> cargv;
    cargv.reserve(args.size() + 1);
    for (std::string& s : args) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) return fail(paths.gameDir, paths.exeDir, "fork 失败: " + std::string(std::strerror(errno)));
    if (pid == 0) {
        std::string oldLd = mc263::fsx::env("LD_LIBRARY_PATH");
        std::string newLd = nativesDir.string() + (oldLd.empty() ? "" : ":" + oldLd);
        ::setenv("LD_LIBRARY_PATH", newLd.c_str(), 1);
        if (chdir(paths.gameDir.c_str()) != 0) { /* 忽略，Java 会自行处理相对路径 */ }
        ::execv(java.c_str(), cargv.data());
        std::println(stderr, "execv 失败: {} ({})", std::strerror(errno), java.string());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    int code = WIFEXITED(status) ? WEXITSTATUS(status)
                                 : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
    logLine(logPath, "exit=" + std::to_string(code));
    if (code != 0) std::println(stderr, "Java 退出码 {}，详情见 {}", code, logPath.string());
    return code;
}
