// modrinth.hpp -- Modrinth 公开 API 的最小封装：搜索 / 版本列表 / 下载
// 不需要 API key，走系统 curl（复用 download.hpp 的 fetchText / fetchOne）
#pragma once

#include "download.hpp"
#include "fs_util.hpp"
#include "mini_json.hpp"

#include <cstdint>
#include <filesystem>
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

struct Version {
    std::string id;
    std::string name;
    std::string number;
    std::vector<std::string> gameVersions;
    std::vector<std::string> loaders;
    std::vector<File> files;
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

inline bool parseVersions(const std::string& text, std::vector<Version>& out, std::string& err) {
    out.clear();
    if (text.empty()) { err = "响应为空"; return false; }
    JValue j = mc263::jsonParse(text, err);
    if (!err.empty()) return false;
    if (!j.isArray()) { err = "响应不是版本数组"; return false; }
    for (const JValue& v : j.array()) {
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
        out.push_back(std::move(x));
    }
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

}  // namespace mc263::modrinth
