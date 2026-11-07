// fetch_263.cpp -- 从 Mojang/CDN 下载 Minecraft 26.3 所需文件到构建目录
//
// 端点（2026-11 实测，见 tests/probe_endpoints.py）:
//   version manifest : https://piston-meta.mojang.com/mc/game/version_manifest_v2.json
//   client jar       : version json -> downloads.client
//   libraries        : https://libraries.minecraft.net/...
//   asset index      : version json -> assetIndex.url
//   assets objects   : https://resources.download.minecraft.net/<hash[:2]>/<hash>
//   Java runtime     : https://launchermeta.mojang.com/v1/products/java-runtime/<id>/all.json
//                     -> linux/java-runtime-epsilon[0].manifest.url -> files[*].downloads.raw
//   Fabric profile   : https://meta.fabricmc.net/v2/versions/loader/<loader>/<game>/profile/json
//
// 只下载公开资源文件，不涉及账号认证；使用者仍需拥有正版 Minecraft 账号。
#include "download.hpp"
#include "fs_util.hpp"
#include "mini_json.hpp"
#include "safe_path.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <print>
#include <set>
#include <string>
#include <sys/utsname.h>
#include <vector>

namespace fs = std::filesystem;
using mc263::JValue;
namespace dl = mc263::dl;
namespace fsx = mc263::fsx;

static constexpr const char* MANIFEST_URL =
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
static constexpr const char* RUNTIME_MANIFEST_URL =
    "https://launchermeta.mojang.com/v1/products/java-runtime/"
    "2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json";
static constexpr const char* FABRIC_LOADERS = "https://meta.fabricmc.net/v2/versions/loader?limit=1";
static constexpr const char* ASSET_CDN = "https://resources.download.minecraft.net/";

static const char* kUsage = R"(mc263-fetch -- 下载 Minecraft 26.3 的游戏文件（供 mc263-build 使用）

用法: mc263-fetch [选项]

  --mc-dir PATH        下载目标目录（默认 ~/.local/share/mc263/gamefiles）
  --version VER        版本 id，或 latest / latest-snapshot（默认 26.3）
  --component NAME     要下载的组件，可重复：jar libraries assets runtime fabric mods
                       （默认 jar libraries assets runtime）
  --mod-url URL        额外下载的 mod / 文件（可重复），放入 <mc-dir>/mods
  --mirror NAME        official(默认) | bmclapi（国内镜像）
  --jobs N             并发下载数 1-16（默认 8）
  --arch ARCH          x86_64 | aarch64（默认本机）
  --cache-dir PATH     元数据缓存（默认 ~/.cache/mc263）
  --timeout SEC        单文件超时（默认 300）
  --limit N            每个阶段只处理前 N 个文件（调试/小流量测试，0=全部）
  --no-cache           忽略本地已存在文件，强制重新校验下载
  --list-versions      列出最近的 release 版本后退出
  --dry-run            只统计任务，不下载
  --json-progress      每行输出一个 JSON 进度对象（供 GUI 解析）
  --allow-partial      允许个别文件失败仍返回 0
  --check-paths        只校验输出路径是否合法，然后退出（不联网、不写文件）
  --help               显示本帮助

路径安全: 默认只允许写入项目目录（沙箱根）；/、/home、/etc、/usr 等一律拒绝。
         可用 MC263_SANDBOX_ROOT 改沙箱根，MC263_ALLOW_OUTSIDE=1 放行沙箱外非敏感路径。

示例:
  mc263-fetch --version 26.3 --mc-dir ~/.local/share/mc263/gamefiles
  mc263-fetch --component fabric --mod-url https://example.com/mod.jar --mirror bmclapi
)";

struct Options {
    std::string mcDir;
    std::string version = "26.3";
    std::string mirror = "official";
    std::string arch;
    std::string cacheDir;
    std::vector<std::string> components;
    std::vector<std::string> modUrls;
    size_t jobs = 8;
    size_t limit = 0;
    int timeout = 300;
    bool onlyMissing = true, dryRun = false, jsonProgress = false;
    bool allowPartial = false, listVersions = false, help = false, checkPaths = false;
};

static std::string detectArch() {
    struct utsname u{};
    if (uname(&u) == 0 && (std::string(u.machine) == "aarch64" || std::string(u.machine) == "arm64"))
        return "aarch64";
    return "x86_64";
}
static std::string toLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
static bool parseArgs(int argc, char** argv, Options& o, std::string& err) {
    // 默认输出全部落在项目沙箱内，绝不写 /home、/etc 等敏感目录
    mc263::safe::Policy pol = mc263::safe::policyFromEnv(argv[0]);
    o.mcDir = fsx::env("MC263_MC_DIR");
    if (o.mcDir.empty()) o.mcDir = (pol.sandboxRoot / "gamefiles").string();
    o.cacheDir = (pol.sandboxRoot / ".cache/fetch").string();
    o.arch = detectArch();
    o.components = {"jar", "libraries", "assets", "runtime"};
    bool componentsExplicit = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i], key = arg, val;
        size_t eq = arg.find('=');
        if (eq != std::string::npos) { key = arg.substr(0, eq); val = arg.substr(eq + 1); }
        auto value = [&]() -> std::string {
            if (!val.empty()) return val;
            if (i + 1 < argc) return argv[++i];
            return {};
        };
        auto need = [&](const std::string& v) {
            if (v.empty()) { err = key + " 缺少参数值"; return false; }
            return true;
        };
        if (key == "--help" || key == "-h") { o.help = true; return true; }
        else if (key == "--mc-dir") { o.mcDir = value(); if (!need(o.mcDir)) return false; }
        else if (key == "--version") { o.version = value(); if (!need(o.version)) return false; }
        else if (key == "--mirror") { o.mirror = value(); if (!need(o.mirror)) return false; }
        else if (key == "--arch") { o.arch = value(); if (!need(o.arch)) return false; }
        else if (key == "--cache-dir") { o.cacheDir = value(); if (!need(o.cacheDir)) return false; }
        else if (key == "--timeout") { o.timeout = std::stoi(value()); }
        else if (key == "--jobs") { o.jobs = static_cast<size_t>(std::stoi(value())); }
        else if (key == "--limit") { o.limit = static_cast<size_t>(std::stoi(value())); }
        else if (key == "--component") {
            std::string c = value(); if (!need(c)) return false;
            if (!componentsExplicit) { o.components.clear(); componentsExplicit = true; }
            // 同时支持逗号分隔
            size_t st = 0;
            while (true) {
                size_t q = c.find(',', st);
                std::string part = c.substr(st, q == std::string::npos ? q : q - st);
                if (!part.empty()) o.components.push_back(part);
                if (q == std::string::npos) break;
                st = q + 1;
            }
        }
        else if (key == "--mod-url") { std::string u = value(); if (!need(u)) return false; o.modUrls.push_back(u); }
        else if (key == "--no-cache") o.onlyMissing = false;
        else if (key == "--list-versions") o.listVersions = true;
        else if (key == "--dry-run") o.dryRun = true;
        else if (key == "--json-progress") o.jsonProgress = true;
        else if (key == "--allow-partial") o.allowPartial = true;
        else if (key == "--check-paths") o.checkPaths = true;
        else { err = "未知参数: " + arg; return false; }
    }
    err.clear();
    return true;
}

static void emit(bool json, const std::string& stage, size_t done, size_t total,
                 const std::string& label, bool ok, const std::string& note) {
    if (json) {
        JValue v = JValue::objectV();
        v.set("stage", JValue::stringV(stage));
        v.set("done", JValue::numberV(static_cast<double>(done)));
        v.set("total", JValue::numberV(static_cast<double>(total)));
        v.set("label", JValue::stringV(label));
        v.set("ok", JValue::booleanV(ok));
        v.set("note", JValue::stringV(note));
        std::string line = mc263::jsonDump(v, 0);
        line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        std::println("{}", line);
        std::fflush(stdout);
    } else if (!ok) {
        std::println(stderr, "  [失败] {}: {}", label, note);
    } else if (done == total || done % 100 == 0) {
        std::println("  {} {}/{}", stage, done, total);
    }
}

static bool libAllowed(const JValue& lib, const std::string& arch) {
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

struct StageResult { size_t total = 0, failed = 0, cached = 0; uint64_t bytes = 0; };

static bool runStage(const std::string& name, std::vector<dl::Task>& tasks, const Options& o,
                     StageResult& result, std::vector<std::string>& failures, std::string& err) {
    if (o.limit > 0 && tasks.size() > o.limit) {
        std::println("  {}: --limit {}，只处理前 {} / {} 个文件", name, o.limit, o.limit, tasks.size());
        tasks.resize(o.limit);
    }
    result.total = tasks.size();
    if (tasks.empty()) { std::println("  {}: 无任务", name); return true; }
    if (o.dryRun) {
        uint64_t bytes = 0;
        for (auto& t : tasks) bytes += fsx::fileSize(t.dest);
        std::println("  {}: {} 个文件待处理（已存在 {} 字节）", name, tasks.size(), bytes);
        return true;
    }
    std::atomic<size_t> cached{0};
    auto cb = [&](size_t done, size_t total, const std::string& label, bool ok, const std::string& note) {
        if (note == "cached") cached.fetch_add(1);
        emit(o.jsonProgress, name, done, total, label, ok, note);
    };
    size_t failed = dl::runTasks(tasks, o.mirror, o.jobs, o.timeout, o.onlyMissing, cb, failures);
    result.failed = failed;
    result.cached = cached.load();
    for (const auto& t : tasks) result.bytes += fsx::fileSize(t.dest);
    if (failed && !o.allowPartial) {
        err = name + " 阶段有 " + std::to_string(failed) + " 个文件下载失败";
        return false;
    }
    std::println("  {}: {} 个文件，缓存命中 {}，失败 {}，共 {}", name, tasks.size(),
                 result.cached, failed, fsx::humanSize(result.bytes));
    return true;
}

int main(int argc, char** argv) {
    Options o;
    std::string err;
    try {
        if (!parseArgs(argc, argv, o, err)) { std::println(stderr, "错误: {}", err); return 2; }
    } catch (const std::exception& e) {
        std::println(stderr, "参数错误: {}", e.what());
        return 2;
    }
    if (o.help) { std::print("{}", kUsage); return 0; }

    // 路径守卫：必须在任何目录创建 / 网络下载之前执行
    mc263::safe::Policy pol = mc263::safe::policyFromEnv(argv[0]);
    std::vector<std::pair<std::string, fs::path>> writes = {
        {"--mc-dir", fs::path(o.mcDir)},
        {"--cache-dir", fs::path(o.cacheDir)},
        {"mods 目录", fs::path(o.mcDir) / "mods"},
    };
    bool pathOk = true;
    for (const auto& [name, p] : writes) {
        auto r = mc263::safe::checkWrite(p, pol);
        std::println("  路径检查 {}: {} -> {}", name, p.string(), r.ok ? "允许" : r.reason);
        if (!r.ok) pathOk = false;
    }
    if (!pathOk) {
        std::println(stderr, "错误: 输出路径被安全策略拒绝（{}）", mc263::safe::describe(pol));
        return 2;
    }
    if (o.checkPaths) {
        std::println("路径检查通过（{}）", mc263::safe::describe(pol));
        return 0;
    }

    std::error_code ec;
    if (!fsx::makeDirs(fs::path(o.cacheDir), err) || !fsx::makeDirs(fs::path(o.mcDir), err)) {
        std::println(stderr, "错误: {}", err);
        return 1;
    }
    std::println("mc263-fetch | 目标 {} | 版本 {} | 镜像 {} | 并发 {}", o.mcDir, o.version, o.mirror, o.jobs);
    if (!o.jsonProgress) std::println("提示: 只下载公开资源文件，使用游戏仍需正版账号。");

    // 1) version manifest
    std::string manifestText;
    if (!dl::fetchText(MANIFEST_URL, o.mirror, fs::path(o.cacheDir), 60, manifestText, err)) {
        std::println(stderr, "错误: 下载版本清单失败: {}", err);
        return 1;
    }
    JValue manifest = mc263::jsonParse(manifestText, err);
    if (!err.empty()) { std::println(stderr, "错误: 解析版本清单失败: {}", err); return 1; }
    const JValue* versions = manifest.find("versions");
    if (!versions || !versions->isArray()) { std::println(stderr, "错误: 版本清单缺少 versions"); return 1; }
    if (o.listVersions) {
        size_t n = 0;
        for (const JValue& v : versions->array()) {
            const JValue* t = v.find("type");
            if (!t || t->str() != "release") continue;
            const JValue* id = v.find("id");
            std::println("  {}", id ? id->str() : "?");
            if (++n >= 20) break;
        }
        return 0;
    }
    std::string wanted = o.version;
    if (wanted == "latest") {
        const JValue* latest = manifest.find("latest");
        const JValue* rel = latest ? latest->find("release") : nullptr;
        wanted = rel ? rel->str() : "26.3";
    } else if (wanted == "latest-snapshot") {
        const JValue* latest = manifest.find("latest");
        const JValue* snap = latest ? latest->find("snapshot") : nullptr;
        wanted = snap ? snap->str() : "26.3";
    }
    const JValue* entry = nullptr;
    for (const JValue& v : versions->array()) {
        const JValue* id = v.find("id");
        if (id && id->str() == wanted) { entry = &v; break; }
    }
    if (!entry) {
        std::println(stderr, "错误: 版本清单中找不到 {}（用 --list-versions 查看）", wanted);
        return 1;
    }
    const JValue* vurl = entry->find("url");
    const JValue* vsha = entry->find("sha1");
    if (!vurl || vurl->str().empty()) { std::println(stderr, "错误: 版本条目缺少 url"); return 1; }
    std::println("  版本清单: {} (release={})", wanted,
                 manifest.find("latest") && manifest.find("latest")->find("release")
                     ? manifest.find("latest")->find("release")->str() : "?");

    fs::path mc(o.mcDir), vdir = mc / "versions" / wanted;
    fs::path versionJson = vdir / (wanted + ".json");
    std::vector<dl::Task> one;
    one.push_back({vurl->str(), versionJson, vsha ? vsha->str() : "", 0644, wanted + ".json"});
    StageResult sr0;
    std::vector<std::string> failures;
    if (!runStage("version", one, o, sr0, failures, err)) { std::println(stderr, "错误: {}", err); return 1; }
    if (o.dryRun) { std::println("--dry-run: 后续阶段请去掉该选项"); return 0; }

    std::string vtext = fsx::readText(versionJson, err);
    JValue vj = mc263::jsonParse(vtext, err);
    if (!err.empty()) { std::println(stderr, "错误: 解析 {} 失败: {}", versionJson.string(), err); return 1; }
    const JValue* aid = vj.find("assetIndex");
    const JValue* javaVer = vj.find("javaVersion");
    std::string assetId = aid && aid->find("id") ? aid->find("id")->str() : wanted;
    std::string component = javaVer && javaVer->find("component")
                                ? javaVer->find("component")->str() : "java-runtime-epsilon";
    std::println("  assetIndex={} | Java 组件={} | 声明库 {} 个", assetId, component,
                 vj.find("libraries") ? vj.find("libraries")->array().size() : 0);

    std::set<std::string> comps(o.components.begin(), o.components.end());
    auto want = [&](const char* c) { return comps.contains(c); };
    size_t totalFailed = 0;

    // 2) client jar
    if (want("jar")) {
        std::vector<dl::Task> tasks;
        const JValue* dlobj = vj.find("downloads");
        const JValue* client = dlobj ? dlobj->find("client") : nullptr;
        if (client) {
            const JValue* u = client->find("url");
            const JValue* s = client->find("sha1");
            if (u) tasks.push_back({u->str(), vdir / (wanted + ".jar"), s ? s->str() : "", 0644,
                                    wanted + ".jar"});
        } else std::println(stderr, "警告: 版本 json 没有 downloads.client");
        StageResult sr;
        if (!runStage("jar", tasks, o, sr, failures, err)) { std::println(stderr, "错误: {}", err); return 1; }
        totalFailed += sr.failed;
    }

    // 3) libraries（含 natives-linux classifier）
    if (want("libraries")) {
        std::vector<dl::Task> tasks;
        std::vector<std::string> classifierKeys;
        if (o.arch == "aarch64") classifierKeys = {"natives-linux-arm64", "natives-linux-aarch64", "natives-linux"};
        else classifierKeys = {"natives-linux", "natives-linux-x86_64"};
        const JValue* libs = vj.find("libraries");
        if (libs && libs->isArray()) {
            for (const JValue& lib : libs->array()) {
                if (!libAllowed(lib, o.arch)) continue;
                const JValue* downloads = lib.find("downloads");
                if (!downloads || !downloads->isObject()) continue;
                auto addArtifact = [&](const JValue* art) {
                    if (!art || !art->isObject()) return;
                    const JValue* path = art->find("path");
                    const JValue* url = art->find("url");
                    const JValue* sha = art->find("sha1");
                    if (!path || !url || path->str().empty()) return;
                    tasks.push_back({url->str(), mc / "libraries" / path->str(),
                                     sha ? sha->str() : "", 0644, path->str()});
                };
                addArtifact(downloads->find("artifact"));
                const JValue* cls = downloads->find("classifiers");
                if (cls && cls->isObject()) {
                    for (const std::string& key : classifierKeys) {
                        const JValue* c = cls->find(key);
                        if (!c) continue;
                        addArtifact(c);
                        break;
                    }
                }
                // 旧格式 natives 字段（1.7.10 等）
                const JValue* natives = lib.find("natives");
                if (natives && natives->isObject() && (!cls || !cls->isObject())) {
                    const JValue* ln = natives->find("linux");
                    if (ln && !ln->str().empty()) {
                        const JValue* nm = lib.find("name");
                        if (nm) {
                            std::string c = ln->str();
                            std::string ph = "${arch}";
                            size_t p;
                            while ((p = c.find(ph)) != std::string::npos)
                                c.replace(p, ph.size(), o.arch == "aarch64" ? "arm64" : "64");
                            // Mojang 旧格式无直接 url，按 maven 路径拼
                            std::vector<std::string> parts;
                            size_t start = 0;
                            while (true) {
                                size_t q = nm->str().find(':', start);
                                if (q == std::string::npos) { parts.push_back(nm->str().substr(start)); break; }
                                parts.push_back(nm->str().substr(start, q - start));
                                start = q + 1;
                            }
                            if (parts.size() >= 3) {
                                std::string g = parts[0], a = parts[1], ver = parts[2];
                                std::string gp;
                                for (char ch : g) gp.push_back(ch == '.' ? '/' : ch);
                                std::string rel = gp + "/" + a + "/" + ver + "/" + a + "-" + ver + "-" + c + ".jar";
                                tasks.push_back({"https://libraries.minecraft.net/" + rel,
                                                 mc / "libraries" / rel, "", 0644, rel});
                            }
                        }
                    }
                }
            }
        }
        StageResult sr;
        if (!runStage("libraries", tasks, o, sr, failures, err)) { std::println(stderr, "错误: {}", err); return 1; }
        totalFailed += sr.failed;
    }

    // 4) assetIndex + assets objects
    if (want("assets")) {
        std::vector<dl::Task> idxTasks;
        const JValue* iu = aid ? aid->find("url") : nullptr;
        const JValue* ish = aid ? aid->find("sha1") : nullptr;
        fs::path idxPath = mc / "assets" / "indexes" / (assetId + ".json");
        if (iu) idxTasks.push_back({iu->str(), idxPath, ish ? ish->str() : "", 0644, assetId + ".json"});
        StageResult srIdx;
        if (!runStage("asset-index", idxTasks, o, srIdx, failures, err)) { std::println(stderr, "错误: {}", err); return 1; }
        std::string idxText = fsx::readText(idxPath, err);
        JValue idx = mc263::jsonParse(idxText, err);
        const JValue* objs = idx.find("objects");
        std::vector<dl::Task> tasks;
        if (objs && objs->isObject()) {
            for (const auto& kv : objs->object()) {
                const JValue* h = kv.second.find("hash");
                if (!h || h->str().size() < 3) continue;
                std::string hash = h->str();
                tasks.push_back({std::string(ASSET_CDN) + hash.substr(0, 2) + "/" + hash,
                                 mc / "assets" / "objects" / hash.substr(0, 2) / hash,
                                 hash, 0644, kv.first});
            }
        } else std::println(stderr, "警告: asset index 里没有 objects");
        StageResult sr;
        if (!runStage("assets", tasks, o, sr, failures, err)) { std::println(stderr, "错误: {}", err); return 1; }
        totalFailed += sr.failed;
    }

    // 5) Java runtime（Mojang 官方 JRE 清单；aarch64 官方暂无对应平台）
    if (want("runtime") && o.arch == "aarch64") {
        std::println(stderr, "警告: 官方 JRE 清单目前只有 linux (x86_64)；aarch64 请用 "
                            "--java-runtime 或系统 Java，已跳过 runtime 下载");
    }
    if (want("runtime") && o.arch != "aarch64") {
        std::string rtText;
        if (!dl::fetchText(RUNTIME_MANIFEST_URL, o.mirror, fs::path(o.cacheDir), 60, rtText, err)) {
            std::println(stderr, "警告: 下载 Java 运行时清单失败: {}（构建时可回退系统 Java）", err);
        } else {
            JValue rt = mc263::jsonParse(rtText, err);
            std::string platform = o.arch == "aarch64" ? "linux-arm64" : "linux";
            const JValue* plat = rt.find(platform);
            if (!plat) { std::println(stderr, "警告: 清单里没有 {} 架构的 JRE，回退 linux", platform); plat = rt.find("linux"); }
            const JValue* cmp = plat ? plat->find(component) : nullptr;
            const JValue* first = (cmp && cmp->isArray() && !cmp->array().empty()) ? &cmp->array()[0] : nullptr;
            const JValue* mf = first ? first->find("manifest") : nullptr;
            const JValue* murl = mf ? mf->find("url") : nullptr;
            if (murl && !murl->str().empty()) {
                std::string mfText;
                if (!dl::fetchText(murl->str(), o.mirror, fs::path(o.cacheDir), 60, mfText, err)) {
                    std::println(stderr, "警告: 下载 JRE 文件清单失败: {}", err);
                } else {
                    JValue files = mc263::jsonParse(mfText, err);
                    const JValue* fobj = files.find("files");
                    std::vector<dl::Task> tasks;
                    fs::path rtRoot = mc / "runtime" / component;
                    if (fobj && fobj->isObject()) {
                        for (const auto& kv : fobj->object()) {
                            const std::string& rel = kv.first;
                            const JValue& f = kv.second;
                            const JValue* type = f.find("type");
                            std::string t = type ? type->str() : "file";
                            if (t == "directory") { fs::create_directories(rtRoot / rel, ec); continue; }
                            if (t == "link") {
                                const JValue* target = f.find("target");
                                if (target) {
                                    fs::path link = rtRoot / rel;
                                    fs::create_directories(link.parent_path(), ec);
                                    fs::remove(link, ec);
                                    fs::create_symlink(fs::path(target->str()), link, ec);
                                }
                                continue;
                            }
                            const JValue* downloads = f.find("downloads");
                            const JValue* raw = downloads ? downloads->find("raw") : nullptr;
                            const JValue* u = raw ? raw->find("url") : nullptr;
                            const JValue* s = raw ? raw->find("sha1") : nullptr;
                            if (!u) continue;
                            const JValue* exe = f.find("executable");
                            tasks.push_back({u->str(), rtRoot / rel, s ? s->str() : "",
                                             exe && exe->boolean() ? 0755u : 0644u, rel});
                        }
                    }
                    StageResult sr;
                    if (!runStage("runtime", tasks, o, sr, failures, err)) {
                        std::println(stderr, "警告: {}（可回退系统 Java）", err);
                        totalFailed += sr.failed;
                    } else {
                        totalFailed += sr.failed;
                        fs::path java = rtRoot / "bin" / "java";
                        if (fsx::isFile(java)) {
                            ::chmod(java.c_str(), 0755);
                            std::println("  内嵌 JRE 就绪: {}", java.string());
                        } else std::println(stderr, "警告: JRE 里没有 bin/java");
                    }
                }
            } else std::println(stderr, "警告: 清单里找不到 {} 的 {} 条目", platform, component);
        }
    }

    // 6) Fabric profile（实验性：官方 meta API 生成版本 json）
    if (want("fabric")) {
        std::string loaderText;
        if (!dl::fetchText(FABRIC_LOADERS, o.mirror, fs::path(o.cacheDir), 30, loaderText, err)) {
            std::println(stderr, "警告: 拉取 Fabric loader 版本失败: {}", err);
        } else {
            JValue loaders = mc263::jsonParse(loaderText, err);
            std::string loader;
            if (loaders.isArray() && !loaders.array().empty()) {
                const JValue* lv = loaders.array()[0].find("version");
                if (lv) loader = lv->str();
            }
            if (!loader.empty()) {
                std::string profileUrl = "https://meta.fabricmc.net/v2/versions/loader/" + loader +
                                         "/" + wanted + "/profile/json";
                std::string profileText;
                if (!dl::fetchText(profileUrl, o.mirror, fs::path(o.cacheDir), 60, profileText, err)) {
                    std::println(stderr, "警告: 下载 Fabric profile 失败: {}", err);
                } else {
                    fs::path fdir = mc / "versions" / (wanted + "+fabric");
                    fs::create_directories(fdir, ec);
                    fsx::writeText(fdir / (wanted + "+fabric.json"), profileText, err);
                    // builder 需要同目录的 .jar；官方 profile 用 inheritsFrom，这里复制一份 client jar
                    fs::path srcJar = vdir / (wanted + ".jar");
                    fs::path dstJar = fdir / (wanted + "+fabric.jar");
                    if (fsx::isFile(srcJar) && !fsx::exists(dstJar)) fsx::copyFile(srcJar, dstJar, err);
                    // profile 里的 libraries 也一并下载
                    JValue prof = mc263::jsonParse(profileText, err);
                    std::vector<dl::Task> tasks;
                    const JValue* libs = prof.find("libraries");
                    if (libs && libs->isArray()) {
                        for (const JValue& lib : libs->array()) {
                            const JValue* u = lib.find("url");
                            const JValue* nm = lib.find("name");
                            const JValue* sha = lib.find("sha1");
                            if (!u || !nm) continue;
                            std::vector<std::string> parts;
                            size_t st = 0;
                            while (true) {
                                size_t q = nm->str().find(':', st);
                                if (q == std::string::npos) { parts.push_back(nm->str().substr(st)); break; }
                                parts.push_back(nm->str().substr(st, q - st));
                                st = q + 1;
                            }
                            if (parts.size() < 3) continue;
                            std::string g = parts[0], a = parts[1], ver = parts[2];
                            std::string gp;
                            for (char ch : g) gp.push_back(ch == '.' ? '/' : ch);
                            std::string rel = gp + "/" + a + "/" + ver + "/" + a + "-" + ver + ".jar";
                            std::string full = u->str();
                            if (!full.empty() && full.back() == '/') full += rel;
                            else if (full.size() < 4 || full.substr(full.size() - 4) != ".jar") full += "/" + rel;
                            tasks.push_back({full, mc / "libraries" / rel,
                                             sha ? sha->str() : "", 0644, rel});
                        }
                    }
                    StageResult sr;
                    runStage("fabric", tasks, o, sr, failures, err);
                    totalFailed += sr.failed;
                    std::println("  Fabric profile: {}（构建时用 --version {}+fabric）", fdir.string(), wanted);
                }
            }
        }
    }

    // 7) 额外 URL（mods / 任意文件）
    if (want("mods") || !o.modUrls.empty()) {
        std::vector<dl::Task> tasks;
        for (const std::string& u : o.modUrls) {
            std::string name = u.substr(u.find_last_of('/') + 1);
            if (name.empty() || name.find('?') != std::string::npos) name = "mod-" + std::to_string(tasks.size()) + ".jar";
            tasks.push_back({u, mc / "mods" / name, "", 0644, name});
        }
        StageResult sr;
        if (!runStage("mods", tasks, o, sr, failures, err)) { std::println(stderr, "错误: {}", err); return 1; }
        totalFailed += sr.failed;
    }

    if (!failures.empty()) {
        std::println(stderr, "\n失败明细（前 10 条）:");
        for (size_t i = 0; i < std::min<size_t>(10, failures.size()); ++i)
            std::println(stderr, "  - {}", failures[i]);
    }
    if (totalFailed > 0 && !o.allowPartial) {
        std::println(stderr, "\n有 {} 个文件下载失败，可用 --allow-partial 或换 --mirror bmclapi 重试", totalFailed);
        return 1;
    }
    std::println("\n完成。现在可以构建单文件：\n  mc263-build --mc-dir {} --version {} --output <路径>",
                 o.mcDir, wanted);
    return 0;
}
