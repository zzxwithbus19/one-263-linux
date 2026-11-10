// launcher_core.hpp -- HMCL 式启动器的纯 C++23 核心（不依赖 Qt，可独立单测）
//
// 目录形态：[appDir/mc263-qt ELF][appDir/minecraft/ 游戏目录][appDir/.mc263/ 配置]
// 职责：路径布局与守卫、配置读写、Java 探测、兼容性查验、启动命令拼装、下载命令拼装。
// Qt 界面只负责展示与后台执行，全部逻辑在这里，便于离线测试。
#pragma once

#include "download.hpp"
#include "fs_util.hpp"
#include "mini_json.hpp"
#include "mini_zip.hpp"
#include "safe_path.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <set>
#include <map>
#include <sstream>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace mc263::core {

namespace fs = std::filesystem;
namespace fsx = mc263::fsx;
using mc263::JValue;

inline std::string detectArch() {
    struct utsname u{};
    if (uname(&u) == 0 && (std::string(u.machine) == "aarch64" || std::string(u.machine) == "arm64"))
        return "aarch64";
    return "x86_64";
}
inline std::string toLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
inline uint64_t freeDiskMb(const fs::path& p) {
    struct statvfs v{};
    if (statvfs(p.c_str(), &v) != 0) return 0;
    return static_cast<uint64_t>(v.f_bavail) * v.f_frsize / (1024 * 1024);
}

// ------------------------------------------------------------ 内存实况 ----
// /proc/meminfo 的几个口径（和 free / 系统监视器一致）：
//   程序占用 = MemTotal - MemAvailable（含内核、页表等不可回收部分）
//   缓存     = Cached + SReclaimable - Shmem（可回收，紧张时会被自动释放）
//   仅可用   = MemAvailable（应用真正能拿到的量，已把缓存的可回收部分算进去）
struct MemInfo {
    uint64_t totalMb = 0;      // 物理内存总量
    uint64_t freeMb = 0;       // MemFree：完全空闲
    uint64_t availableMb = 0;  // MemAvailable：可用（含可回收缓存）
    uint64_t buffersMb = 0;    // Buffers
    uint64_t cachedMb = 0;     // 文件页缓存（可回收）
    uint64_t usedMb = 0;       // 其他程序实际占用 = total - available
};

inline MemInfo parseMemInfo(const std::string& text) {
    MemInfo m;
    uint64_t freeMb = 0, availMb = 0, buffersMb = 0, cachedMb = 0, sreclaimMb = 0, shmemMb = 0;
    std::istringstream in(text);
    std::string key, rest;
    uint64_t value = 0;
    while (in >> key >> value) {
        std::getline(in, rest);  // 吃掉行尾的 " kB" / 注释
        if (key == "MemTotal:") m.totalMb = value / 1024;
        else if (key == "MemFree:") freeMb = value / 1024;
        else if (key == "MemAvailable:") availMb = value / 1024;
        else if (key == "Buffers:") buffersMb = value / 1024;
        else if (key == "Cached:") cachedMb = value / 1024;
        else if (key == "SReclaimable:") sreclaimMb = value / 1024;
        else if (key == "Shmem:") shmemMb = value / 1024;
    }
    uint64_t cache = cachedMb + sreclaimMb;
    cache = cache > shmemMb ? cache - shmemMb : cachedMb;
    m.freeMb = freeMb;
    m.buffersMb = buffersMb;
    m.cachedMb = cache;
    // 老内核（< 3.14）没有 MemAvailable，用 free + cache 估算
    if (availMb == 0) availMb = freeMb + cache;
    if (m.totalMb > 0 && availMb > m.totalMb) availMb = m.totalMb;
    m.availableMb = availMb;
    m.usedMb = m.totalMb > availMb ? m.totalMb - availMb : 0;
    return m;
}

inline MemInfo readMemInfo() {
    std::ifstream in("/proc/meminfo");
    std::stringstream ss;
    ss << in.rdbuf();
    return parseMemInfo(ss.str());
}

inline int systemMemoryMb() {
    auto m = readMemInfo();
    return static_cast<int>(m.totalMb);
}

// ------------------------------------------------- 显示后端（Qt 平台插件）----
// Linux 桌面已普遍只提供 Wayland 会话（KDE Plasma 6 默认如此，XWayland 可能完全缺席）。
// 创建 QApplication 之前在这里决定 QT_QPA_PLATFORM：
//   1. 用户已设置（或命令行 -platform）→ 一律尊重，不干预；
//   2. Wayland 会话且装了 wayland 插件 → wayland（原生，无 XWayland 转发）；
//   3. Wayland 会话但只有 xcb → 回退 xcb（XWayland），并提示补装插件；
//   4. 两个插件都没有 → 直接给出可读错误，避免 Qt 自己 qFatal 出一堆看不懂的日志。
struct PlatformPick {
    std::string plugin;  // 要写入 QT_QPA_PLATFORM 的值；空 = 不干预 / 不可用
    std::string note;    // 面向用户的一句话说明
    bool fatal = false;  // true = 没有任何可用后端
};

inline PlatformPick choosePlatform(const std::string& forced, bool waylandSession,
                                   bool hasWayland, bool hasXcb) {
    if (!forced.empty()) return {forced, "沿用已设置的 QT_QPA_PLATFORM=" + forced, false};
    if (waylandSession) {
        if (hasWayland) return {"wayland", "Wayland 会话 → 原生 wayland 后端", false};
        if (hasXcb) return {"xcb", "Wayland 会话缺少 qt6-qtwayland → 经 XWayland 回退 xcb", false};
        return {"", "Wayland 会话下找不到 wayland / xcb 平台插件：请安装 qt6-qtwayland"
                    "（Fedora）或 libQt6WaylandClient6（openSUSE）", true};
    }
    if (hasXcb) return {"xcb", "X11 会话 → xcb 后端", false};
    if (hasWayland) return {"wayland", "没有 X11 显示，改用 wayland 后端", false};
    return {"", "找不到可用的 Qt 平台插件（wayland / xcb 均缺失）", true};
}

// ------------------------------------------------------------------ 布局 ----
struct Layout {
    fs::path appDir;        // ELF 所在目录
    fs::path gameDir;       // 游戏目录（默认 appDir/minecraft）
    fs::path configDir;     // appDir/.mc263
    fs::path configFile;    // configDir/config.json
    fs::path versionsDir, librariesDir, assetsDir, runtimeDir, modsDir, nativesDir;
    mc263::safe::Policy policy;

    static Layout at(const fs::path& root, const std::string& gameDirOverride = "") {
        Layout l;
        l.appDir = root;
        // 应用目录本身就是可信沙箱根（用户要求游戏目录建在应用目录下）
        std::string envRoot = fsx::env("MC263_SANDBOX_ROOT");
        l.policy.sandboxRoot = envRoot.empty() ? l.appDir : mc263::safe::normalize(fs::path(envRoot));
        l.policy.allowOutside = fsx::env("MC263_ALLOW_OUTSIDE") == "1";
        l.gameDir = gameDirOverride.empty()
                        ? l.appDir / "minecraft"
                        : mc263::safe::normalize(fs::path(gameDirOverride));
        // 用户在界面里自己选的游戏目录可能落在 ~ 下（沙箱根之外），显式选择就认它，
        // 但仍然拒绝 /etc、/usr 这类系统关键路径。
        if (!gameDirOverride.empty() && !mc263::safe::isCritical(l.gameDir))
            l.policy.extraRoots.push_back(l.gameDir);
        l.configDir = l.appDir / ".mc263";
        l.configFile = l.configDir / "config.json";
        l.versionsDir = l.gameDir / "versions";
        l.librariesDir = l.gameDir / "libraries";
        l.assetsDir = l.gameDir / "assets";
        l.runtimeDir = l.gameDir / "runtime";
        l.modsDir = l.gameDir / "mods";
        l.nativesDir = l.gameDir / "natives";
        return l;
    }
    // exe 所在目录通常就是应用目录；若那里不可写（例如装进 /usr/bin），
    // 调用方应改用 XDG 数据目录，用 at() 指定 root。
    static Layout detect(const fs::path& exe, const std::string& gameDirOverride = "") {
        return at(mc263::safe::normalize(exe).parent_path(), gameDirOverride);
    }
    bool writable(std::string& err) const {
        for (const fs::path& p : {gameDir, configDir}) {
            auto r = mc263::safe::checkWrite(p, policy);
            if (!r.ok) { err = r.reason; return false; }
        }
        err.clear();
        return true;
    }
    bool ensure(std::string& err) const {
        if (!writable(err)) return false;
        for (const fs::path& p : {gameDir, configDir, versionsDir, librariesDir,
                                  assetsDir / "indexes", assetsDir / "objects",
                                  runtimeDir, modsDir, nativesDir}) {
            if (!fsx::makeDirs(p, err)) return false;
        }
        return true;
    }
    fs::path versionDir(const std::string& version) const { return versionsDir / version; }
    fs::path versionJson(const std::string& version) const {
        return versionsDir / version / (version + ".json");
    }
    fs::path versionJar(const std::string& version) const {
        return versionsDir / version / (version + ".jar");
    }
};

// ------------------------------------------------------------------ 配置 ----
struct Config {
    std::string version = "26.3";
    std::string username;
    std::string javaPath;
    std::string mirror = "official";
    std::string arch = detectArch();
    std::string gameDir;  // 空 = 用应用目录下的 minecraft/
    bool autoMemory = true;  // 默认交给 JVM 自己决定堆大小（不传 -Xmx/-Xms）
    int memoryMb = 4096;
    int minMemoryMb = 0;
    int jobs = 10;
    bool fabric = false;
    std::string profile;  // 加载器 profile 名（如 fabric），空 = 原版
    std::string language;  // 游戏语言（en_us / zh_cn…）；空 = 跟随系统
    // 配色来源：auto = 检测到 Noctalia 色板就用它，否则跟随系统；system = 只用系统调色板；
    // noctalia = 强制读 Noctalia 的 Material You 色板（读不到就退回系统）
    std::string themeSource = "auto";
    std::vector<std::string> extraJvmArgs;
    std::vector<std::string> components = {"jar", "libraries", "assets", "runtime"};

    JValue toJson() const {
        JValue j = JValue::objectV();
        j.set("version", JValue::stringV(version));
        j.set("username", JValue::stringV(username));
        j.set("javaPath", JValue::stringV(javaPath));
        j.set("mirror", JValue::stringV(mirror));
        j.set("arch", JValue::stringV(arch));
        j.set("gameDir", JValue::stringV(gameDir));
        j.set("autoMemory", JValue::booleanV(autoMemory));
        j.set("memoryMb", JValue::numberV(memoryMb));
        j.set("minMemoryMb", JValue::numberV(minMemoryMb));
        j.set("jobs", JValue::numberV(jobs));
        j.set("fabric", JValue::booleanV(fabric));
        j.set("profile", JValue::stringV(profile));
        j.set("language", JValue::stringV(language));
        j.set("themeSource", JValue::stringV(themeSource));
        JValue a = JValue::arrayV();
        for (const auto& s : extraJvmArgs) a.push(JValue::stringV(s));
        j.set("extraJvmArgs", a);
        JValue c = JValue::arrayV();
        for (const auto& s : components) c.push(JValue::stringV(s));
        j.set("components", c);
        return j;
    }
    static Config fromJson(const JValue& j) {
        Config c;
        auto s = [&](const char* k, std::string& out) {
            const JValue* v = j.find(k);
            if (v && v->isString()) out = v->str();
        };
        s("version", c.version); s("username", c.username); s("javaPath", c.javaPath);
        s("mirror", c.mirror); s("arch", c.arch); s("gameDir", c.gameDir);
        const JValue* am = j.find("autoMemory"); if (am) c.autoMemory = am->boolean(true);
        const JValue* m = j.find("memoryMb"); if (m) c.memoryMb = static_cast<int>(m->number(4096));
        const JValue* mi = j.find("minMemoryMb"); if (mi) c.minMemoryMb = static_cast<int>(mi->number(0));
        const JValue* jobs = j.find("jobs"); if (jobs) c.jobs = static_cast<int>(jobs->number(10));
        const JValue* f = j.find("fabric"); if (f) c.fabric = f->boolean(false);
        s("profile", c.profile); s("language", c.language); s("themeSource", c.themeSource);
        if (c.themeSource != "auto" && c.themeSource != "system" && c.themeSource != "noctalia")
            c.themeSource = "auto";
        const JValue* a = j.find("extraJvmArgs");
        if (a && a->isArray()) { c.extraJvmArgs.clear(); for (const auto& v : a->array()) if (v.isString()) c.extraJvmArgs.push_back(v.str()); }
        const JValue* cc = j.find("components");
        if (cc && cc->isArray()) { c.components.clear(); for (const auto& v : cc->array()) if (v.isString()) c.components.push_back(v.str()); }
        if (c.version.empty()) c.version = "26.3";
        if (c.arch.empty()) c.arch = detectArch();
        if (c.memoryMb < 256) c.memoryMb = 256;
        return c;
    }
    bool load(const fs::path& p, std::string& err) {
        std::string text = fsx::readText(p, err);
        if (!err.empty()) return false;
        JValue j = mc263::jsonParse(text, err);
        if (!err.empty()) return false;
        *this = fromJson(j);
        return true;
    }
    bool save(const fs::path& p, std::string& err) const {
        if (!fsx::makeDirs(p.parent_path(), err)) return false;
        return fsx::writeText(p, mc263::jsonDump(toJson()), err);
    }
    std::string xmxString() const { return "-Xmx" + std::to_string(memoryMb) + "M"; }
    std::string xmsString() const { return minMemoryMb > 0 ? "-Xms" + std::to_string(minMemoryMb) + "M" : ""; }
};

// 版本目录名：装了加载器就是 <版本>+<profile>
inline std::string versionDirName(const Config& c) {
    if (!c.profile.empty()) return c.version + "+" + c.profile;
    return c.fabric ? c.version + "+fabric" : c.version;
}

// 版本隔离后的 gameDir：每个版本一个目录，存档 / mods / 资源包互不干扰
inline fs::path isolatedGameDir(const Layout& l, const Config& c) {
    return l.versionsDir / versionDirName(c);
}

struct GameDirInfo {
    fs::path path;
    int versions = 0;      // versions/ 下带同名 json 的目录数
    int mods = 0;          // mods/*.jar 数量
    bool current = false;  // 是否是当前使用的目录
    bool exists = false;
};

// 扫一圈可能被当成游戏目录的位置（ELF 旁边的 minecraft/、rpm 装完的 XDG 目录、~/.minecraft 等）
// 只列真实存在的目录，避免列出「虚空目录」；当前生效目录例外——首次运行它可能还没建出来
inline std::vector<GameDirInfo> scanGameDirs(const Layout& l, const std::string& currentCfg) {
    auto existsDir = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_directory(p, ec);
    };

    std::vector<fs::path> cands;
    if (!currentCfg.empty() && (existsDir(currentCfg) || currentCfg == l.gameDir.string()))
        cands.push_back(currentCfg);
    cands.push_back(l.gameDir);  // 永远保留：正在用的目录，哪怕还没创建
    const fs::path home = fsx::env("HOME").empty() ? fs::path(".") : fs::path(fsx::env("HOME"));
    if (existsDir(home / ".minecraft")) cands.push_back(home / ".minecraft");
    const std::string xdg = fsx::env("XDG_DATA_HOME");
    const fs::path xdgDir =
        (xdg.empty() ? home / ".local" / "share" : fs::path(xdg)) / "mc263" / "minecraft";
    if (existsDir(xdgDir)) cands.push_back(xdgDir);

    std::vector<fs::path> seen;
    std::vector<GameDirInfo> out;
    for (const auto& raw : cands) {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(raw, ec);
        if (ec) p = raw;
        bool dup = false;
        for (const auto& s : seen) if (s == p) { dup = true; break; }
        if (dup) continue;
        seen.push_back(p);

        GameDirInfo info;
        info.path = p;
        info.exists = fsx::isDir(p);
        if (info.exists) {
            if (fsx::isDir(p / "versions")) {
                for (fs::directory_iterator it(p / "versions", ec), end; !ec && it != end; it.increment(ec)) {
                    if (!it->is_directory()) continue;
                    const std::string n = it->path().filename().string();
                    if (fsx::isFile(it->path() / (n + ".json"))) info.versions++;
                }
            }
            if (fsx::isDir(p / "mods")) {
                for (fs::directory_iterator it(p / "mods", ec), end; !ec && it != end; it.increment(ec)) {
                    if (it->is_regular_file() && it->path().extension() == ".jar") info.mods++;
                }
            }
        }
        out.push_back(std::move(info));
    }
    // 当前生效的目录打标记
    std::error_code ec;
    const fs::path cur = fs::weakly_canonical(l.gameDir, ec);
    for (auto& g : out) if (g.path == cur) g.current = true;
    return out;
}

// ------------------------------------------------------------ 游戏语言 ----
// 系统 locale → Minecraft 语言代码；识别不了就退回 en_us
inline std::string detectSystemLanguage() {
    static const char* vars[] = {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"};
    std::string raw;
    for (const char* v : vars) {
        const std::string val = fsx::env(v);
        if (val.empty() || val == "C" || val == "POSIX") continue;
        raw = val;
        break;
    }
    if (raw.empty()) return "en_us";

    const size_t colon = raw.find(':');          // LANGUAGE="zh_CN:en_US"
    if (colon != std::string::npos) raw = raw.substr(0, colon);
    const size_t dot = raw.find('.');            // zh_CN.UTF-8
    if (dot != std::string::npos) raw = raw.substr(0, dot);
    const size_t at = raw.find('@');             // sr_RS@latin
    if (at != std::string::npos) raw = raw.substr(0, at);

    std::string loc;
    for (char ch : raw) loc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    if (loc.empty()) return "en_us";

    static const char* known[] = {
        "en_us", "en_gb", "zh_cn", "zh_tw", "de_de", "es_es", "fr_fr", "it_it", "ja_jp", "ko_kr",
        "pt_br", "pt_pt", "ru_ru", "nl_nl", "pl_pl", "sv_se", "tr_tr", "da_dk", "fi_fi", "nb_no",
        "nn_no", "cs_cz", "hu_hu", "el_gr", "he_il", "uk_ua", "ro_ro", "bg_bg", "sk_sk", "sl_si",
        "hr_hr", "sr_rs", "lt_lt", "lv_lv", "et_ee", "ca_es", "eu_eu", "gl_es", "id_id", "ms_my",
        "th_th", "vi_vn", "ar_sa", "fa_ir", "hi_in", "bn_bd", "ta_in", "ur_pk", "be_by", "ka_ge",
        "az_az", "mk_mk", "sq_al", "bs_ba"};
    for (const char* k : known)
        if (loc == k) return loc;

    // 只有语言没有地区时的常见映射
    const std::string lang = loc.substr(0, loc.find('_'));
    static const std::pair<const char*, const char*> alias[] = {
        {"zh", "zh_cn"},   {"en", "en_us"},   {"ja", "ja_jp"},   {"ko", "ko_kr"},
        {"de", "de_de"},   {"fr", "fr_fr"},   {"es", "es_es"},   {"it", "it_it"},
        {"pt", "pt_br"},   {"ru", "ru_ru"},   {"nl", "nl_nl"},   {"pl", "pl_pl"},
        {"tr", "tr_tr"},   {"sv", "sv_se"},   {"da", "da_dk"},   {"fi", "fi_fi"},
        {"cs", "cs_cz"},   {"hu", "hu_hu"},   {"el", "el_gr"},   {"he", "he_il"},
        {"uk", "uk_ua"},   {"ro", "ro_ro"},   {"bg", "bg_bg"},   {"sk", "sk_sk"},
        {"sl", "sl_si"},   {"hr", "hr_hr"},   {"sr", "sr_rs"},   {"lt", "lt_lt"},
        {"lv", "lv_lv"},   {"et", "et_ee"},   {"ca", "ca_es"},   {"eu", "eu_eu"},
        {"gl", "gl_es"},   {"id", "id_id"},   {"ms", "ms_my"},   {"th", "th_th"},
        {"vi", "vi_vn"},   {"ar", "ar_sa"},   {"fa", "fa_ir"},   {"hi", "hi_in"},
        {"bn", "bn_bd"},   {"ta", "ta_in"},   {"ur", "ur_pk"},   {"be", "be_by"},
        {"ka", "ka_ge"},   {"az", "az_az"},   {"mk", "mk_mk"},   {"sq", "sq_al"},
        {"bs", "bs_ba"}};
    for (const auto& kv : alias)
        if (lang == kv.first) return kv.second;
    return "en_us";
}

// ---------------------------------------------------------------- 加载器 ----
struct LoaderVersion {
    std::string version;
    bool stable = false;
};

inline std::string loaderBase(const std::string& loader) {
    return loader == "quilt" ? "https://meta.quiltmc.org/v3" : "https://meta.fabricmc.net/v2";
}

inline bool loaderVersions(const std::string& loader, const fs::path& cacheDir,
                           std::vector<LoaderVersion>& out, std::string& err) {
    out.clear();
    std::string text;
    if (!mc263::dl::fetchText(loaderBase(loader) + "/versions/loader", "", cacheDir, 25, text, err)) return false;
    std::string jerr;
    JValue j = mc263::jsonParse(text, jerr);
    if (!jerr.empty()) { err = "解析加载器版本失败: " + jerr; return false; }
    if (!j.isArray()) { err = "加载器版本响应不是数组"; return false; }
    for (const JValue& v : j.array()) {
        LoaderVersion lv;
        if (const JValue* x = v.find("version"); x && x->isString()) lv.version = x->str();
        if (const JValue* st = v.find("stable"); st) lv.stable = st->boolean(false);
        if (!lv.version.empty()) out.push_back(std::move(lv));
    }
    err.clear();
    return !out.empty();
}

// group:artifact:version[:classifier] -> Maven 仓库里的相对路径
inline std::string mavenPath(const std::string& coord) {
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : coord) {
        if (ch == ':') { parts.push_back(cur); cur.clear(); } else cur.push_back(ch);
    }
    parts.push_back(cur);
    if (parts.size() < 3) return {};
    std::string group = parts[0], artifact = parts[1], ver = parts[2];
    const std::string classifier = parts.size() > 3 ? parts[3] : "";
    for (char& ch : group) if (ch == '.') ch = '/';
    return group + "/" + artifact + "/" + ver + "/" + artifact + "-" + ver +
           (classifier.empty() ? "" : "-" + classifier) + ".jar";
}

// 装 Fabric / Quilt 加载器：抓 profile json，与原版 json 合并后写入 versions/<ver>+<loader>/
inline bool installLoader(const Layout& l, Config& c, const std::string& loader,
                          const std::string& loaderVer,
                          const std::function<void(const std::string&)>& note, std::string& err) {
    const fs::path vanilla = l.versionJson(c.version);
    if (!fsx::isFile(vanilla)) {
        err = "先下载原版 " + c.version + "（要用它的 json 作基底）";
        return false;
    }
    const std::string base = loaderBase(loader);
    std::string profText;
    if (!mc263::dl::fetchText(base + "/versions/loader/" + c.version + "/" + loaderVer + "/profile/json",
                              "", l.configDir / "cache", 30, profText, err))
        return false;
    std::string jerr;
    JValue prof = mc263::jsonParse(profText, jerr);
    if (!jerr.empty()) { err = "profile json 解析失败: " + jerr; return false; }

    std::string baseText = fsx::readText(vanilla, err);
    if (!err.empty()) return false;
    JValue json = mc263::jsonParse(baseText, jerr);
    if (!jerr.empty()) { err = "原版 json 解析失败: " + jerr; return false; }

    const std::string dirName = c.version + "+" + loader;
    if (const JValue* mc = prof.find("mainClass"); mc && mc->isString())
        json.set("mainClass", *mc);
    json.set("id", JValue::stringV(dirName));

    JValue libs = JValue::arrayV();
    if (const JValue* vl = json.find("libraries"); vl && vl->isArray())
        for (const JValue& x : vl->array()) libs.push(x);

    std::vector<mc263::dl::Task> tasks;
    if (const JValue* pl = prof.find("libraries"); pl && pl->isArray()) {
        for (const JValue& lib : pl->array()) {
            const JValue* nm = lib.find("name");
            if (!nm || !nm->isString()) continue;
            const JValue* urlv = lib.find("url");
            const std::string repo = (urlv && urlv->isString() && !urlv->str().empty())
                                         ? urlv->str() : "https://maven.fabricmc.net/";
            const std::string path = mavenPath(nm->str());
            if (path.empty()) continue;
            const std::string url = repo + (repo.back() == '/' ? "" : "/") + path;

            JValue entry = JValue::objectV();
            entry.set("name", JValue::stringV(nm->str()));
            JValue art = JValue::objectV();
            art.set("path", JValue::stringV(path));
            art.set("url", JValue::stringV(url));
            JValue dl = JValue::objectV();
            dl.set("artifact", art);
            entry.set("downloads", dl);
            libs.push(entry);

            tasks.push_back(mc263::dl::Task{url, l.librariesDir / path, "", 0644, path});
        }
    }
    json.set("libraries", libs);

    // 写入版本目录
    const fs::path dir = l.versionsDir / dirName;
    if (!fsx::makeDirs(dir, err)) return false;
    if (!fsx::writeText(dir / (dirName + ".json"), mc263::jsonDump(json), err)) return false;

    if (note) note("下载加载器依赖 " + std::to_string(tasks.size()) + " 个");
    std::vector<std::string> failures;
    mc263::dl::runTasks(tasks, "", 8, 180, true, nullptr, failures);
    if (!failures.empty()) {
        err = "有 " + std::to_string(failures.size()) + " 个依赖下载失败，例如 " + failures.front();
        return false;
    }

    c.profile = loader;
    c.fabric = (loader == "fabric");
    err.clear();
    return true;
}

// ------------------------------------------------------------------ Java ----
struct JavaInfo {
    fs::path path;
    std::string version;
    std::string source;
    int major = 0;
    bool valid = false;
};

inline int captureJavaVersion(const fs::path& java, std::string& versionOut) {
    int fds[2];
    if (::pipe(fds) != 0) return -1;
    std::vector<std::string> a = {java.string(), "-version"};
    std::vector<char*> av;
    for (auto& s : a) av.push_back(s.data());
    av.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) { ::close(fds[0]); ::close(fds[1]); return -1; }
    if (pid == 0) {
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[0]); ::close(fds[1]);
        ::execv(java.c_str(), av.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    FILE* f = ::fdopen(fds[0], "r");
    char buf[512];
    std::string all;
    while (std::fgets(buf, sizeof(buf), f)) all += buf;
    ::fclose(f);
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    versionOut = all.substr(0, all.find('\n'));
    // 解析 "openjdk version \"25.0.1\"" / "java version \"1.8.0_412\""
    size_t q = all.find('"');
    if (q != std::string::npos) {
        size_t e = all.find('"', q + 1);
        if (e != std::string::npos) {
            std::string v = all.substr(q + 1, e - q - 1);
            versionOut = v;
            size_t dot = v.find('.');
            std::string first = dot == std::string::npos ? v : v.substr(0, dot);
            if (first == "1" && dot != std::string::npos) {
                size_t d2 = v.find('.', dot + 1);
                first = d2 == std::string::npos ? v.substr(dot + 1) : v.substr(dot + 1, d2 - dot - 1);
            }
            return std::atoi(first.c_str());
        }
    }
    return 0;
}

inline std::vector<JavaInfo> findJava(const Layout& l, const Config& c) {
    std::vector<fs::path> candidates;
    if (!c.javaPath.empty()) candidates.push_back(fs::path(c.javaPath));
    // Mojang 内嵌 JRE
    if (fsx::isDir(l.runtimeDir)) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(l.runtimeDir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().filename() == "java" && it->path().parent_path().filename() == "bin")
                candidates.push_back(it->path());
        }
    }
    std::string jh = fsx::env("JAVA_HOME");
    if (!jh.empty()) candidates.push_back(fs::path(jh) / "bin/java");
    std::string path = fsx::env("PATH");
    size_t st = 0;
    while (st < path.size()) {
        size_t e = path.find(':', st);
        std::string dir = path.substr(st, e == std::string::npos ? e : e - st);
        if (!dir.empty()) candidates.push_back(fs::path(dir) / "java");
        if (e == std::string::npos) break;
        st = e + 1;
    }
    if (fsx::isDir("/usr/lib/jvm")) {
        std::error_code ec;
        for (fs::directory_iterator it("/usr/lib/jvm", ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory()) candidates.push_back(it->path() / "bin/java");
        }
    }
    candidates.push_back("/usr/bin/java");

    std::vector<JavaInfo> out;
    std::vector<std::string> seen;
    for (const auto& p : candidates) {
        std::error_code ec;
        fs::path abs = fs::weakly_canonical(p, ec);
        if (ec || !fsx::isFile(p)) continue;
        std::string key = abs.string();
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        JavaInfo ji;
        ji.path = p;
        std::string ver;
        ji.major = captureJavaVersion(p, ver);
        ji.version = ver;
        ji.valid = ji.major > 0 && ::access(p.c_str(), X_OK) == 0;
        std::string ps = p.string();
        if (!c.javaPath.empty() && ps == c.javaPath) ji.source = "配置指定";
        else if (ps.find(l.runtimeDir.string()) != std::string::npos) ji.source = "Mojang JRE";
        else if (!jh.empty() && ps.rfind(fs::path(jh).string(), 0) == 0) ji.source = "JAVA_HOME";
        else ji.source = "系统 PATH / JVM";
        out.push_back(ji);
    }
    std::sort(out.begin(), out.end(), [](const JavaInfo& a, const JavaInfo& b) { return a.major > b.major; });
    return out;
}

// 版本需要的 Java 主版本：Minecraft 26.x 要 25、1.20+ 要 21、1.17~1.19 要 17、更早要 8。
// 优先读已下载的 version json 里 javaVersion.majorVersion，没下载时按版本号推断。
inline int javaMajorForVersionName(const std::string& v) {
    int a = 0, b = -1, c = -1;
    if (std::sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c) < 2) return 25;
    if (a >= 26) return 25;
    if (a == 1) {
        if (b >= 20) return 21;
        if (b >= 17) return 17;
        return 8;
    }
    return 25;
}

inline int requiredJavaMajor(const Layout& l, const std::string& version) {
    const int guess = javaMajorForVersionName(version);
    std::string err;
    const std::string text = fsx::readText(l.versionJson(version), err);
    if (!err.empty()) return guess;
    JValue j = mc263::jsonParse(text, err);
    if (!err.empty()) return guess;
    const JValue* jv = j.find("javaVersion");
    const JValue* jm = jv ? jv->find("majorVersion") : nullptr;
    return jm ? static_cast<int>(jm->number(guess)) : guess;
}

// -------------------------------------------------------------- version ----
inline bool libAllowed(const JValue& lib, const std::string& arch) {
    const JValue* rules = lib.find("rules");
    if (!rules || !rules->isArray() || rules->array().empty()) return true;
    bool allowed = false;
    for (const JValue& r : rules->array()) {
        bool match = true;
        const JValue* os = r.find("os");
        if (os && os->isObject()) {
            const JValue* n = os->find("name");
            std::string name = n ? n->str() : "";
            if (!name.empty() && name != "linux" && name != "unix" && name != "unknown") match = false;
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

struct VersionMeta {
    JValue json;
    std::string mainClass, assetIndex, id;
    int javaMajor = 0;
    std::vector<std::string> classpath;
    std::vector<fs::path> nativeJars;
    std::string error;
    bool ok() const { return error.empty(); }
};

inline VersionMeta loadVersion(const Layout& l, const Config& c) {
    VersionMeta m;
    fs::path vj = l.versionJson(versionDirName(c));
    std::string err;
    std::string text = fsx::readText(vj, err);
    if (!err.empty()) { m.error = "缺少版本 json: " + vj.string(); return m; }
    m.json = mc263::jsonParse(text, err);
    if (!err.empty()) { m.error = "版本 json 解析失败: " + err; return m; }
    const JValue* id = m.json.find("id");
    m.id = id ? id->str() : c.version;
    const JValue* mc = m.json.find("mainClass");
    m.mainClass = mc ? mc->str() : "net.minecraft.client.main.Main";
    const JValue* ai = m.json.find("assetIndex");
    const JValue* aid = ai ? ai->find("id") : nullptr;
    m.assetIndex = aid ? aid->str() : c.version;
    const JValue* jv = m.json.find("javaVersion");
    const JValue* jm = jv ? jv->find("majorVersion") : nullptr;
    m.javaMajor = jm ? static_cast<int>(jm->number(21)) : 21;

    const JValue* libs = m.json.find("libraries");
    std::vector<std::string> keys = c.arch == "aarch64"
        ? std::vector<std::string>{"natives-linux-arm64", "natives-linux-aarch64", "natives-linux"}
        : std::vector<std::string>{"natives-linux", "natives-linux-x86_64"};
    if (libs && libs->isArray()) {
        for (const JValue& lib : libs->array()) {
            if (!libAllowed(lib, c.arch)) continue;
            const JValue* d = lib.find("downloads");
            if (!d || !d->isObject()) continue;
            const JValue* art = d->find("artifact");
            if (art && art->isObject()) {
                const JValue* path = art->find("path");
                if (path && !path->str().empty()) m.classpath.push_back((l.librariesDir / path->str()).string());
            }
            const JValue* cls = d->find("classifiers");
            if (cls && cls->isObject()) {
                for (const auto& key : keys) {
                    const JValue* cv = cls->find(key);
                    if (!cv) continue;
                    const JValue* p2 = cv->find("path");
                    if (p2 && !p2->str().empty()) m.nativeJars.push_back(l.librariesDir / p2->str());
                    break;
                }
            }
        }
    }
    m.classpath.push_back(l.versionJar(versionDirName(c)).string());
    return m;
}

// ------------------------------------------------------------ 兼容性查验 ----
struct CheckItem {
    std::string name;
    bool ok = false;
    std::string detail;
};

inline std::vector<CheckItem> compatibility(const Layout& l, const Config& c,
                                            const std::vector<JavaInfo>& javas,
                                            bool checkNetwork = false) {
    std::vector<CheckItem> out;
    auto add = [&](std::string n, bool ok, std::string d) { out.push_back({std::move(n), ok, std::move(d)}); };
    std::string perr;
    bool pathOk = l.writable(perr);
    add("路径安全", pathOk, pathOk ? "游戏目录与配置目录均在允许的沙箱内" : perr);

    std::string machine;
    struct utsname u{};
    if (uname(&u) == 0) machine = u.machine;
    add("系统架构", c.arch == detectArch(), machine + " / 目标 " + c.arch);

    int mem = systemMemoryMb();
    add("物理内存", mem >= 2048, mem ? std::to_string(mem) + " MB" : "无法读取 /proc/meminfo");

    uint64_t disk = freeDiskMb(l.appDir);
    add("可用磁盘", disk >= 2048, std::to_string(disk) + " MB");

    JavaInfo best;
    for (const auto& j : javas) if (j.valid) { best = j; break; }
    VersionMeta vm = loadVersion(l, c);
    int need = vm.ok() ? vm.javaMajor : 21;
    add("Java 运行时", best.valid && best.major >= need,
        best.valid ? best.version + " (" + best.source + ", 需 >= " + std::to_string(need) + ")"
                   : "未找到可用 Java，需 >= " + std::to_string(need));

    add("版本 json", vm.ok(), vm.ok() ? vm.id + " / " + vm.mainClass : vm.error);
    fs::path jar = l.versionJar(c.fabric ? c.version + "+fabric" : c.version);
    add("客户端 jar", fsx::isFile(jar), jar.string());
    fs::path idx2 = l.assetsDir / "indexes" / ((vm.ok() ? vm.assetIndex : c.version) + ".json");
    add("资源索引", fsx::isFile(idx2), idx2.string());

    size_t missing = 0;
    for (const auto& p : vm.classpath) if (!fsx::isFile(fs::path(p))) ++missing;
    add("依赖库", vm.ok() && missing == 0,
        vm.ok() ? "共 " + std::to_string(vm.classpath.size()) + " 项，缺失 " + std::to_string(missing)
                : "版本 json 未就绪");

    bool nativesOk = !vm.nativeJars.empty();
    for (const auto& p : vm.nativeJars) if (!fsx::isFile(p)) nativesOk = false;
    add("Linux natives", nativesOk,
        vm.nativeJars.empty() ? "版本 json 里没有 natives-linux classifier" : std::to_string(vm.nativeJars.size()) + " 个 natives jar");

    auto hasLib = [](const char* name) {
        std::vector<fs::path> dirs = {"/usr/lib64", "/usr/lib", "/lib64", "/lib"};
        for (const auto& d : dirs) if (fsx::isFile(d / name)) return true;
        return false;
    };
    bool gl = hasLib("libGL.so.1") || hasLib("libGL.so");
    bool x11 = hasLib("libX11.so.6") || hasLib("libX11.so");
    bool al = hasLib("libasound.so.2") || hasLib("libasound.so");
    add("图形/音频库", gl && x11 && al,
        std::string("libGL ") + (gl ? "✓" : "✗") + " · libX11 " + (x11 ? "✓" : "✗") +
        " · libasound " + (al ? "✓" : "✗"));

    if (checkNetwork) {
        std::vector<std::string> a = {"curl", "-sS", "-o", "/dev/null", "-w", "%{http_code}",
                                      "--max-time", "8",
                                      "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"};
        std::vector<char*> av;
        for (auto& s : a) av.push_back(s.data());
        av.push_back(nullptr);
        int fds[2];
        ::pipe(fds);
        pid_t pid = ::fork();
        if (pid == 0) {
            ::dup2(fds[1], STDOUT_FILENO);
            int dn = ::open("/dev/null", O_WRONLY);
            if (dn >= 0) { ::dup2(dn, STDERR_FILENO); ::close(dn); }
            ::close(fds[0]); ::close(fds[1]);
            ::execvp("curl", av.data());
            ::_exit(127);
        }
        ::close(fds[1]);
        char b[32] = {0};
        ssize_t n = ::read(fds[0], b, sizeof(b) - 1);
        (void)n;
        ::close(fds[0]);
        int st = 0;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        add("官方下载源", std::string(b) == "200", "HTTP " + std::string(b));
    }
    return out;
}

// -------------------------------------------------------------- 启动命令 ----
struct LaunchPlan {
    std::vector<std::string> argv;
    std::string classpath;
    std::string mainClass;
    fs::path workDir;
    std::string error;
    bool ok() const { return error.empty() && !argv.empty(); }
};

inline bool extractNatives(const std::vector<fs::path>& jars, const fs::path& dir, std::string& err) {
    if (!fsx::makeDirs(dir, err)) return false;
    for (const auto& jar : jars) {
        if (!fsx::isFile(jar)) continue;
        mc263::zip::Archive ar;
        if (!ar.openFile(jar, err)) { err.clear(); continue; }
        for (const auto& e : ar.entries()) {
            std::string name = e.name;
            size_t slash = name.find_last_of('/');
            std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
            if (base.find(".so") == std::string::npos) continue;
            std::vector<uint8_t> data;
            std::string e2;
            if (!ar.read(e, data, e2)) continue;
            fs::path out = dir / base;
            std::ofstream f(out, std::ios::binary);
            if (f) { f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())); }
            ::chmod(out.c_str(), 0755);
        }
    }
    err.clear();
    return true;
}

inline LaunchPlan buildLaunchPlan(const Layout& l, const Config& c, const JavaInfo& java) {
    LaunchPlan p;
    p.workDir = l.gameDir;
    if (!java.valid) { p.error = "没有可用的 Java 运行时"; return p; }
    VersionMeta vm = loadVersion(l, c);
    if (!vm.ok()) { p.error = vm.error; return p; }
    std::string err;
    if (!l.ensure(err)) { p.error = "无法创建运行目录: " + err; return p; }
    fs::path natives = l.nativesDir / versionDirName(c);
    if (!extractNatives(vm.nativeJars, natives, err)) { p.error = err; return p; }

    // 版本隔离（始终开启，不提供关闭）：每个版本自己的 gameDir，存档 / mods / 资源包互不影响
    const fs::path isolated = isolatedGameDir(l, c);
    if (!fsx::makeDirs(isolated, err)) { p.error = "无法创建版本目录: " + err; return p; }
    for (const char* sub : {"mods", "saves", "resourcepacks", "shaderpacks", "logs"}) {
        if (!fsx::makeDirs(isolated / sub, err)) { p.error = "无法创建 " + std::string(sub) + ": " + err; return p; }
    }
    p.workDir = isolated;

    for (const auto& s : vm.classpath) {
        if (!p.classpath.empty()) p.classpath += ":";
        p.classpath += s;
    }
    p.mainClass = vm.mainClass;
    p.argv.push_back(java.path.string());
    if (!c.autoMemory) {  // 自动模式下不传 -Xmx/-Xms，让 JVM 按物理内存自己定
        if (c.minMemoryMb > 0) p.argv.push_back(c.xmsString());
        p.argv.push_back(c.xmxString());
    }
    for (const auto& a : c.extraJvmArgs) p.argv.push_back(a);
    p.argv.push_back("-Djava.library.path=" + natives.string());
    p.argv.push_back("-Dorg.lwjgl.librarypath=" + natives.string());
    p.argv.push_back("-Djna.tmpdir=" + natives.string());
    p.argv.push_back("-Dio.netty.native.workdir=" + natives.string());
    p.argv.push_back("-cp");
    p.argv.push_back(p.classpath);
    p.argv.push_back(vm.mainClass);
    p.argv.insert(p.argv.end(), {
        "--username", c.username.empty() ? "Player" : c.username,
        "--version", vm.id,
        "--gameDir", isolated.string(),
        "--assetsDir", l.assetsDir.string(),
        "--assetIndex", vm.assetIndex,
        "--language", c.language.empty() ? detectSystemLanguage() : c.language,
        "--accessToken", "0",
        "--userType", "legacy",
        "--versionType", "release",
    });
    return p;
}

// ------------------------------------------------------------ 本地版本 ----
inline std::vector<std::string> availableVersions(const Layout& l) {
    std::vector<std::string> out;
    if (!fsx::isDir(l.versionsDir)) return out;
    std::error_code ec;
    for (fs::directory_iterator it(l.versionsDir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory()) continue;
        std::string name = it->path().filename().string();
        if (fsx::isFile(it->path() / (name + ".json"))) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// -------------------------------------------------------------- 在线下载 ----
// Qt 应用内嵌下载能力：只依赖系统 curl CLI，不再依赖外部 mc263-fetch 二进制。
struct DownloadProgress {
    std::string stage, label, note;
    size_t done = 0, total = 0;
    bool ok = true;
};
using ProgressFn = std::function<void(const DownloadProgress&)>;
struct DownloadResult {
    bool ok = false;
    size_t downloaded = 0, cached = 0, failed = 0;
    std::string error;
};

inline std::vector<std::string> onlineVersions(const std::string& mirror,
                                               const fs::path& cacheDir, std::string& err) {
    std::string text;
    if (!mc263::dl::fetchText("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json",
                              mirror, cacheDir, 30, text, err)) return {};
    JValue j = mc263::jsonParse(text, err);
    if (!err.empty()) return {};
    std::vector<std::string> out;
    const JValue* vs = j.find("versions");
    if (vs && vs->isArray()) {
        for (const JValue& v : vs->array()) {
            const JValue* t = v.find("type");
            const JValue* id = v.find("id");
            if (t && t->str() == "release" && id) {
                out.push_back(id->str());
                if (out.size() >= 20) break;
            }
        }
    }
    return out;
}

inline DownloadResult downloadAll(const Layout& l, const Config& c, const ProgressFn& onProgress) {
    DownloadResult result;
    std::string err;
    if (!l.ensure(err)) { result.error = err; return result; }
    fs::path cache = l.configDir / "download-cache";
    if (!fsx::makeDirs(cache, err)) { result.error = err; return result; }
    std::set<std::string> comps(c.components.begin(), c.components.end());
    auto want = [&](const char* k) { return comps.contains(k); };
    std::atomic<size_t> downloaded{0}, cached{0}, failed{0};
    size_t jobs = std::max(1, std::min(16, c.jobs));

    auto run = [&](const char* stage, std::vector<mc263::dl::Task>& tasks) {
        if (tasks.empty()) return;
        std::vector<std::string> failures;
        auto cb = [&](size_t d, size_t t, const std::string& label, bool ok, const std::string& note) {
            if (note == "cached") cached.fetch_add(1);
            else if (ok) downloaded.fetch_add(1);
            if (onProgress) onProgress({stage, label, note, d, t, ok});
        };
        failed.fetch_add(mc263::dl::runTasks(tasks, c.mirror, jobs, 300, true, cb, failures));
    };
    auto emit = [&](const char* stage, const std::string& label, bool ok, const std::string& note) {
        if (onProgress) onProgress({stage, label, note, 1, 1, ok});
    };

    // 1) 版本清单与版本 json
    std::string manifestText;
    if (!mc263::dl::fetchText("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json",
                              c.mirror, cache, 60, manifestText, err)) {
        result.error = "版本清单下载失败: " + err; return result;
    }
    JValue manifest = mc263::jsonParse(manifestText, err);
    if (!err.empty()) { result.error = "版本清单解析失败: " + err; return result; }
    std::string wanted = c.version;
    if (wanted == "latest" || wanted == "latest-snapshot") {
        const JValue* latest = manifest.find("latest");
        const JValue* rel =
            latest ? latest->find(wanted == "latest-snapshot" ? "snapshot" : "release") : nullptr;
        wanted = rel ? rel->str() : "26.3";
    }
    const JValue* versions = manifest.find("versions");
    const JValue* entry = nullptr;
    if (versions && versions->isArray()) {
        for (const JValue& v : versions->array()) {
            const JValue* id = v.find("id");
            if (id && id->str() == wanted) { entry = &v; break; }
        }
    }
    if (!entry) { result.error = "版本清单中找不到 " + wanted; return result; }
    const JValue* vurl = entry->find("url");
    const JValue* vsha = entry->find("sha1");
    std::vector<mc263::dl::Task> metaTasks;
    fs::path vdir = l.versionDir(wanted);
    metaTasks.push_back({vurl->str(), l.versionJson(wanted), vsha ? vsha->str() : "", 0644, wanted + ".json"});
    run("version", metaTasks);
    if (failed.load() > 0) { result.error = "版本 json 下载失败"; return result; }

    std::string vtext = fsx::readText(l.versionJson(wanted), err);
    JValue vj = mc263::jsonParse(vtext, err);
    if (!err.empty()) { result.error = "版本 json 解析失败: " + err; return result; }
    const JValue* aid = vj.find("assetIndex");
    std::string assetId = aid && aid->find("id") ? aid->find("id")->str() : wanted;
    emit("version", "已解析版本 " + wanted + " / assetIndex " + assetId, true, "");

    // 2) 客户端 jar + libraries（含 natives-linux）
    if (want("jar") || want("libraries")) {
        std::vector<mc263::dl::Task> tasks;
        const JValue* dls = vj.find("downloads");
        const JValue* client = dls ? dls->find("client") : nullptr;
        if (want("jar") && client) {
            const JValue* u = client->find("url");
            const JValue* s = client->find("sha1");
            if (u) tasks.push_back({u->str(), l.versionJar(wanted), s ? s->str() : "", 0644, wanted + ".jar"});
        }
        if (want("libraries")) {
            std::vector<std::string> keys = c.arch == "aarch64"
                ? std::vector<std::string>{"natives-linux-arm64", "natives-linux-aarch64", "natives-linux"}
                : std::vector<std::string>{"natives-linux", "natives-linux-x86_64"};
            const JValue* libs = vj.find("libraries");
            if (libs && libs->isArray()) {
                for (const JValue& lib : libs->array()) {
                    if (!libAllowed(lib, c.arch)) continue;
                    const JValue* d = lib.find("downloads");
                    if (!d || !d->isObject()) continue;
                    auto add = [&](const JValue* art) {
                        if (!art || !art->isObject()) return;
                        const JValue* path = art->find("path");
                        const JValue* url = art->find("url");
                        const JValue* sha = art->find("sha1");
                        if (path && url && !path->str().empty())
                            tasks.push_back({url->str(), l.librariesDir / path->str(),
                                             sha ? sha->str() : "", 0644, path->str()});
                    };
                    add(d->find("artifact"));
                    const JValue* cls = d->find("classifiers");
                    if (cls && cls->isObject()) {
                        for (const auto& k : keys) {
                            const JValue* cv = cls->find(k);
                            if (cv) { add(cv); break; }
                        }
                    }
                }
            }
        }
        run("libraries", tasks);
    }

    // 3) asset index + 资源对象
    if (want("assets") && aid) {
        const JValue* iu = aid->find("url");
        const JValue* ish = aid->find("sha1");
        fs::path idxPath = l.assetsDir / "indexes" / (assetId + ".json");
        std::vector<mc263::dl::Task> idxTasks;
        if (iu) idxTasks.push_back({iu->str(), idxPath, ish ? ish->str() : "", 0644, assetId + ".json"});
        run("asset-index", idxTasks);
        std::string idxText = fsx::readText(idxPath, err);
        JValue idx = mc263::jsonParse(idxText, err);
        const JValue* objs = idx.find("objects");
        std::vector<mc263::dl::Task> tasks;
        if (objs && objs->isObject()) {
            for (const auto& kv : objs->object()) {
                const JValue* h = kv.second.find("hash");
                if (!h || h->str().size() < 3) continue;
                std::string hash = h->str();
                tasks.push_back({"https://resources.download.minecraft.net/" + hash.substr(0, 2) + "/" + hash,
                                 l.assetsDir / "objects" / hash.substr(0, 2) / hash,
                                 hash, 0644, kv.first});
            }
        }
        run("assets", tasks);
    }

    // 4) Mojang 官方 Java 运行时
    if (want("runtime") && c.arch != "aarch64") {
        std::string rtText;
        if (!mc263::dl::fetchText("https://launchermeta.mojang.com/v1/products/java-runtime/"
                                  "2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json",
                                  c.mirror, cache, 60, rtText, err)) {
            emit("runtime", "JRE 清单下载失败，可稍后使用系统 Java", false, err);
        } else {
            JValue rt = mc263::jsonParse(rtText, err);
            const JValue* jv = vj.find("javaVersion");
            const JValue* cmpName = jv ? jv->find("component") : nullptr;
            std::string component = cmpName ? cmpName->str() : "java-runtime-epsilon";
            const JValue* plat = rt.find("linux");
            const JValue* cmp = plat ? plat->find(component) : nullptr;
            const JValue* first = (cmp && cmp->isArray() && !cmp->array().empty()) ? &cmp->array()[0] : nullptr;
            const JValue* mf = first ? first->find("manifest") : nullptr;
            const JValue* murl = mf ? mf->find("url") : nullptr;
            if (murl) {
                std::string mfText;
                if (mc263::dl::fetchText(murl->str(), c.mirror, cache, 60, mfText, err)) {
                    JValue files = mc263::jsonParse(mfText, err);
                    const JValue* fobj = files.find("files");
                    fs::path rtRoot = l.runtimeDir / component;
                    std::vector<mc263::dl::Task> tasks;
                    if (fobj && fobj->isObject()) {
                        for (const auto& kv : fobj->object()) {
                            const JValue* type = kv.second.find("type");
                            std::string t = type ? type->str() : "file";
                            if (t == "directory") { fsx::makeDirs(rtRoot / kv.first, err); continue; }
                            const JValue* downloads = kv.second.find("downloads");
                            const JValue* raw = downloads ? downloads->find("raw") : nullptr;
                            const JValue* u = raw ? raw->find("url") : nullptr;
                            const JValue* s = raw ? raw->find("sha1") : nullptr;
                            if (!u) continue;
                            const JValue* exe = kv.second.find("executable");
                            tasks.push_back({u->str(), rtRoot / kv.first, s ? s->str() : "",
                                             exe && exe->boolean() ? 0755u : 0644u, kv.first});
                        }
                    }
                    run("runtime", tasks);
                    fs::path java = rtRoot / "bin" / "java";
                    if (fsx::isFile(java)) ::chmod(java.c_str(), 0755);
                }
            }
        }
    }

    result.downloaded = downloaded.load();
    result.cached = cached.load();
    result.failed = failed.load();
    result.ok = result.failed == 0;
    if (!result.ok) result.error = std::to_string(result.failed) + " 个文件下载失败";
    return result;
}


// ------------------------------------------------------ 主题来源（Noctalia）----
// Noctalia（Quickshell 桌面外壳）把 Material You 色板渲染成一份扁平 TOML：
//   primary = "#004890"
//   background = "#f9f9ff"
// 本机这份在 ~/.cache/nyxuri/palette.toml（由 Noctalia 的 user template 渲染）。
// 启动器直接读它就能跟壁纸 / 明暗模式同步，不需要链接任何 M3 生成库。

inline std::string trimCopy(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool isHexColor(const std::string& s) {
    if (s.size() != 7 && s.size() != 9) return false;  // #RRGGBB 或带 alpha 的 #RRGGBBAA
    if (s[0] != '#') return false;
    for (std::size_t i = 1; i < s.size(); ++i)
        if (hexNibble(s[i]) < 0) return false;
    return true;
}

// 0..255 的感知亮度；非法值返回 -1
inline int hexLuma(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return -1;
    int v[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        int hi = hexNibble(hex[1 + i * 2]), lo = hexNibble(hex[2 + i * 2]);
        if (hi < 0 || lo < 0) return -1;
        v[i] = hi * 16 + lo;
    }
    return (v[0] * 299 + v[1] * 587 + v[2] * 114) / 1000;
}

struct M3Palette {
    bool ok = false;
    std::string path;
    std::map<std::string, std::string> colors;

    std::string get(const std::string& key, const std::string& fallback = "") const {
        auto it = colors.find(key);
        return (it != colors.end() && !it->second.empty()) ? it->second : fallback;
    }
    // 只认「够用」的色板：背景、正文、强调色齐了才算数
    bool valid() const {
        return ok && !get("background").empty() && !get("on_surface").empty() &&
               !get("primary").empty();
    }
    bool dark() const {
        const int l = hexLuma(get("background", get("surface")));
        return l >= 0 && l < 128;
    }
};

inline M3Palette parseM3Palette(const std::string& text, const std::string& path = "") {
    M3Palette p;
    p.path = path;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trimCopy(line);
        if (line.empty() || line[0] == '#') continue;  // 注释
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trimCopy(line.substr(0, eq));
        std::string val = trimCopy(line.substr(eq + 1));
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'')) {
            const char q = val.front();
            if (val.back() == q) val = val.substr(1, val.size() - 2);
        }
        if (key.empty() || !isHexColor(val)) continue;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        p.colors[key] = val;
    }
    p.ok = !p.colors.empty();
    return p;
}

// 找色板的顺序：MC263_PALETTE 覆盖 → nyxuri 缓存 → noctalia 缓存 → noctalia 配置目录
inline std::vector<std::string> paletteCandidates() {
    std::vector<std::string> out;
    const std::string forced = fsx::env("MC263_PALETTE");
    if (!forced.empty()) out.push_back(forced);
    std::string cache = fsx::env("XDG_CACHE_HOME");
    if (cache.empty()) cache = fsx::home() + "/.cache";
    std::string cfg = fsx::env("XDG_CONFIG_HOME");
    if (cfg.empty()) cfg = fsx::home() + "/.config";
    out.push_back(cache + "/nyxuri/palette.toml");
    out.push_back(cache + "/noctalia/palette.toml");
    out.push_back(cfg + "/noctalia/palette.toml");
    return out;
}

inline M3Palette loadM3Palette(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(path, ec)) return {};
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseM3Palette(ss.str(), path);
}

inline M3Palette findM3Palette() {
    for (const auto& path : paletteCandidates()) {
        M3Palette p = loadM3Palette(path);
        if (p.valid()) return p;
    }
    return {};
}

// niri 会话：NIRI_SOCKET 最可靠；没有时再从 XDG_CURRENT_DESKTOP / XDG_SESSION_DESKTOP 认
inline bool isNiriSession() {
    if (!fsx::env("NIRI_SOCKET").empty()) return true;
    std::string d = fsx::env("XDG_CURRENT_DESKTOP") + " " + fsx::env("XDG_SESSION_DESKTOP");
    std::transform(d.begin(), d.end(), d.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return d.find("niri") != std::string::npos;
}

} // namespace mc263::core
