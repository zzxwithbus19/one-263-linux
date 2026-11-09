// modrinth.hpp -- Modrinth 公开 API 的最小封装：搜索 / 版本列表 / 下载
// 不需要 API key，走系统 curl（复用 download.hpp 的 fetchText / fetchOne）
#pragma once

#include "download.hpp"
#include "fs_util.hpp"
#include "mini_json.hpp"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace mc263::modrinth {

namespace fs = std::filesystem;
using mc263::JValue;

struct Hit {
    std::string id;  // project_id（搜索不到时退回 slug）
    std::string title;
    std::string author;
    std::string description;
    uint64_t downloads = 0;
};

struct File {
    std::string url;
    std::string filename;
    std::string sha1;
    uint64_t size = 0;
    bool primary = false;
};

// Modrinth 的 dependencies：type = required / optional / incompatible / embedded。
// project_id 为空时说明它不是 Modrinth 上的项目（只有 file_name 提示），只能手动下载。
struct Dependency {
    std::string projectId;
    std::string versionId;
    std::string fileName;
    std::string type;
};

struct Version {
    std::string id;
    std::string name;
    std::string number;
    std::vector<std::string> gameVersions;
    std::vector<std::string> loaders;
    std::vector<File> files;
    std::vector<Dependency> dependencies;
};

struct Project {
    std::string id, slug, title, projectType;
};

inline std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

static inline std::string field(const JValue& o, const char* k) {
    const JValue* v = o.find(k);
    return (v && v->isString()) ? v->str() : std::string();
}

inline bool parseSearch(const std::string& text, std::vector<Hit>& out, std::string& err) {
    out.clear();
    if (text.empty()) { err = "响应为空"; return false; }
    JValue j = mc263::jsonParse(text, err);
    if (!err.empty()) return false;
    const JValue* hits = j.find("hits");
    if (!hits || !hits->isArray()) { err = "响应里没有 hits 数组"; return false; }
    for (const JValue& h : hits->array()) {
        Hit x;
        x.id = field(h, "project_id");
        if (x.id.empty()) x.id = field(h, "slug");
        x.title = field(h, "title");
        x.author = field(h, "author");
        x.description = field(h, "description");
        if (const JValue* d = h.find("downloads")) x.downloads = static_cast<uint64_t>(d->number(0));
        if (!x.id.empty()) out.push_back(std::move(x));
    }
    err.clear();
    return true;
}

inline Version parseVersionObject(const JValue& v) {
    Version x;
    x.id = field(v, "id");
    x.name = field(v, "name");
    x.number = field(v, "version_number");
    if (const JValue* gv = v.find("game_versions"); gv && gv->isArray())
        for (const JValue& s : gv->array()) if (s.isString()) x.gameVersions.push_back(s.str());
    if (const JValue* ld = v.find("loaders"); ld && ld->isArray())
        for (const JValue& s : ld->array()) if (s.isString()) x.loaders.push_back(s.str());
    if (const JValue* fl = v.find("files"); fl && fl->isArray()) {
        for (const JValue& f : fl->array()) {
            File file;
            file.url = field(f, "url");
            file.filename = field(f, "filename");
            if (const JValue* sz = f.find("size")) file.size = static_cast<uint64_t>(sz->number(0));
            if (const JValue* pr = f.find("primary")) file.primary = pr->boolean(false);
            if (const JValue* hs = f.find("hashes")) file.sha1 = field(*hs, "sha1");
            if (!file.url.empty()) x.files.push_back(std::move(file));
        }
    }
    if (const JValue* dp = v.find("dependencies"); dp && dp->isArray()) {
        for (const JValue& d : dp->array()) {
            Dependency dep;
            dep.projectId = field(d, "project_id");
            dep.versionId = field(d, "version_id");
            dep.fileName = field(d, "file_name");
            dep.type = field(d, "dependency_type");
            if (dep.type.empty()) dep.type = "required";
            if (!dep.projectId.empty() || !dep.fileName.empty()) x.dependencies.push_back(std::move(dep));
        }
    }
    return x;
}

inline bool parseVersions(const std::string& text, std::vector<Version>& out, std::string& err) {
    out.clear();
    if (text.empty()) { err = "响应为空"; return false; }
    JValue j = mc263::jsonParse(text, err);
    if (!err.empty()) return false;
    if (!j.isArray()) { err = "响应不是版本数组"; return false; }
    for (const JValue& v : j.array()) out.push_back(parseVersionObject(v));
    err.clear();
    return true;
}

// 选主文件；没有 primary 标记就取第一个 .jar
inline const File* primaryFile(const Version& v) {
    for (const File& f : v.files) if (f.primary) return &f;
    for (const File& f : v.files)
        if (f.filename.ends_with(".jar")) return &f;
    return v.files.empty() ? nullptr : &v.files.front();
}

inline bool search(const std::string& query, const std::string& loader, const std::string& gameVersion,
                   const std::string& sort, const fs::path& cacheDir,
                   std::vector<Hit>& out, std::string& err) {
    std::string facets = "[[\"project_type:mod\"]";
    if (!loader.empty()) facets += ",[\"categories:" + loader + "\"]";
    if (!gameVersion.empty()) facets += ",[\"versions:" + gameVersion + "\"]";
    facets += "]";
    const std::string index = sort.empty() ? "downloads" : sort;
    const std::string url = "https://api.modrinth.com/v2/search?limit=20&index=" + index +
                            "&query=" + urlEncode(query) + "&facets=" + urlEncode(facets);
    std::string text;
    if (!mc263::dl::fetchText(url, "", cacheDir, 25, text, err)) return false;
    return parseSearch(text, out, err);
}

inline bool versions(const std::string& project, const std::string& loader, const std::string& gameVersion,
                     const fs::path& cacheDir, std::vector<Version>& out, std::string& err) {
    std::string url = "https://api.modrinth.com/v2/project/" + urlEncode(project) + "/version";
    std::string sep = "?";
    if (!loader.empty()) { url += sep + "loaders=" + urlEncode("[\"" + loader + "\"]"); sep = "&"; }
    if (!gameVersion.empty()) url += sep + "game_versions=" + urlEncode("[\"" + gameVersion + "\"]");
    std::string text;
    if (!mc263::dl::fetchText(url, "", cacheDir, 25, text, err)) return false;
    return parseVersions(text, out, err);
}

inline bool project(const std::string& idOrSlug, const fs::path& cacheDir, Project& out, std::string& err) {
    const std::string url = "https://api.modrinth.com/v2/project/" + urlEncode(idOrSlug);
    std::string text;
    if (!mc263::dl::fetchText(url, "", cacheDir, 25, text, err)) return false;
    JValue j = mc263::jsonParse(text, err);
    if (!err.empty()) return false;
    out = Project{};
    out.id = field(j, "id");
    out.slug = field(j, "slug");
    out.title = field(j, "title");
    out.projectType = field(j, "project_type");
    if (out.title.empty()) { err = "项目不存在或 API 拒绝访问"; return false; }
    return true;
}

inline bool versionById(const std::string& versionId, const fs::path& cacheDir, Version& out, std::string& err) {
    const std::string url = "https://api.modrinth.com/v2/version/" + urlEncode(versionId);
    std::string text;
    if (!mc263::dl::fetchText(url, "", cacheDir, 25, text, err)) return false;
    JValue j = mc263::jsonParse(text, err);
    if (!err.empty()) return false;
    out = parseVersionObject(j);
    if (out.id.empty()) { err = "版本不存在"; return false; }
    return true;
}

inline std::string projectUrl(const Project& p) {
    if (!p.slug.empty()) return "https://modrinth.com/mod/" + p.slug;
    if (!p.id.empty()) return "https://modrinth.com/mod/" + p.id;
    return "https://modrinth.com/mods";
}

// 一条前置的解析结果：resolved 的能直接下载，manual 的要用户自己去网页下
struct DepItem {
    std::string projectId;
    std::string versionId;
    std::string versionNumber;
    std::string title;
    std::string fileName;   // 作者只给了文件名时用
    std::string url;        // 手动下载时打开的网页
    std::string reason;     // 为什么得手动下
    File file;              // resolved 时的主文件
};

struct DepPlan {
    std::vector<DepItem> resolved;
    std::vector<DepItem> manual;
};

// 广度优先把 required 前置补齐（optional / embedded / incompatible 跳过），最多 6 层。
// 拿不到项目信息、没有适配版本或没有主文件的，归到 manual 里，由界面提示手动下载。
inline bool resolveDependencies(const Version& root, const std::string& loader,
                                const std::string& gameVersion, const fs::path& cacheDir,
                                DepPlan& plan, std::string& err) {
    plan = DepPlan{};
    std::vector<Dependency> queue = root.dependencies;
    std::set<std::string> seen;
    for (int depth = 0; depth < 6 && !queue.empty(); ++depth) {
        std::vector<Dependency> next;
        for (const Dependency& d : queue) {
            if (d.type != "required") continue;  // optional / embedded / incompatible 不装
            if (!d.projectId.empty() && !seen.insert(d.projectId).second) continue;  // 防环

            DepItem item;
            item.projectId = d.projectId;
            item.fileName = d.fileName;

            if (d.projectId.empty()) {
                item.title = d.fileName.empty() ? "未知前置" : d.fileName;
                item.reason = "Modrinth 上没有这个前置（作者只留了文件名）";
                item.url = "https://modrinth.com/mods?q=" + urlEncode(d.fileName);
                plan.manual.push_back(std::move(item));
                continue;
            }

            Project p;
            std::string perr;
            if (project(d.projectId, cacheDir, p, perr)) {
                item.title = p.title;
                item.url = projectUrl(p);
            } else {
                item.title = d.projectId;
                item.url = "https://modrinth.com/mod/" + d.projectId;
            }

            Version v;
            std::string verr;
            bool got = false;
            if (!d.versionId.empty()) got = versionById(d.versionId, cacheDir, v, verr);
            if (!got) {
                std::vector<Version> list;
                std::string lerr;
                if (versions(d.projectId, loader, gameVersion, cacheDir, list, lerr) && !list.empty()) {
                    v = list.front();
                    got = true;
                } else if (verr.empty()) {
                    verr = lerr;
                }
            }
            const File* f = got ? primaryFile(v) : nullptr;
            if (!f) {
                item.reason = got ? "没有可下载的主文件"
                                  : "没有适配 " + gameVersion + "/" + loader + " 的版本";
                plan.manual.push_back(std::move(item));
                continue;
            }

            item.versionId = v.id;
            item.versionNumber = v.number;
            item.file = *f;
            plan.resolved.push_back(std::move(item));

            for (const Dependency& nd : v.dependencies)
                if (nd.type == "required") next.push_back(nd);
        }
        queue.swap(next);
    }
    err.clear();
    return true;
}

}  // namespace mc263::modrinth
