// fs_util.hpp -- 文件系统小工具（C++17 / POSIX，header-only）
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mc263 {
namespace fsx {

namespace fs = std::filesystem;

inline std::string env(const std::string& key, const std::string& def = "") {
    const char* v = std::getenv(key.c_str());
    return (v && *v) ? std::string(v) : def;
}
inline std::string home() {
    std::string h = env("HOME");
    if (!h.empty()) return h;
    // 兜底：passwd
    const char* p = std::getenv("USER");
    return p ? std::string("/home/") + p : std::string("/tmp");
}

// C++23 简化：直接用 std::string 的成员函数，保留这两个名字供旧调用点使用
inline bool endsWith(const std::string& s, const std::string& suffix) {
    return s.ends_with(suffix);
}
inline bool contains(const std::string& s, const std::string& needle) {
    return s.contains(needle);
}
inline bool exists(const fs::path& p) { return fs::exists(p); }
inline bool isFile(const fs::path& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
inline bool isDir(const fs::path& p) { std::error_code ec; return fs::is_directory(p, ec); }

inline uint64_t fileSize(const fs::path& p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    return ec ? 0 : static_cast<uint64_t>(sz);
}
inline uint64_t dirSize(const fs::path& p) {
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) total += fileSize(it->path());
    }
    return total;
}
inline size_t countFiles(const fs::path& p, const std::string& ext = "") {
    size_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && (ext.empty() || endsWith(it->path().filename().string(), ext))) ++n;
    }
    return n;
}
inline std::string humanSize(uint64_t b) {
    char buf[64];
    if (b >= 1024ull * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f GB", b / 1073741824.0);
    else if (b >= 1024ull * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
    else if (b >= 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(b));
    return buf;
}

inline bool makeDirs(const fs::path& p, std::string& err) {
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) { err = "创建目录失败 " + p.string() + ": " + ec.message(); return false; }
    err.clear();
    return true;
}
inline bool removeTree(const fs::path& p, std::string& err) {
    std::error_code ec;
    if (!fs::exists(p, ec)) { err.clear(); return true; }
    fs::remove_all(p, ec);
    if (ec) { err = "删除失败 " + p.string() + ": " + ec.message(); return false; }
    err.clear();
    return true;
}

inline uint32_t unixMode(const fs::path& p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return 0100644;
    uint32_t m = st.st_mode & 07777;
    return m ? m : 0100644;
}

inline bool copyFile(const fs::path& src, const fs::path& dst, std::string& err, bool preserveMode = true) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (!fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec) || ec) {
        err = "复制失败 " + src.string() + " -> " + dst.string() + ": " + ec.message();
        return false;
    }
    if (preserveMode) ::chmod(dst.c_str(), unixMode(src) & 0777);
    err.clear();
    return true;
}

struct CopyStats { uint64_t files = 0, bytes = 0; };

// keep(relativePosixPath, isDir) -> true 表示保留；返回 false 且是目录时整棵子树跳过
inline bool copyTree(const fs::path& src, const fs::path& dst,
                     const std::function<bool(const std::string&, bool)>& keep,
                     CopyStats& stats, std::string& err) {
    if (!isDir(src)) { err = "源目录不存在: " + src.string(); return false; }
    if (!makeDirs(dst, err)) return false;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        std::string rel = fs::relative(p, src, ec).generic_string();
        if (ec) { err = "计算相对路径失败: " + p.string(); return false; }
        bool dir = it->is_directory(ec);
        if (!keep(rel, dir)) {
            if (dir) it.disable_recursion_pending();
            continue;
        }
        fs::path target = dst / fs::path(rel);
        if (dir) {
            if (!makeDirs(target, err)) return false;
        } else {
            if (!copyFile(p, target, err)) return false;
            ++stats.files;
            stats.bytes += fileSize(p);
        }
    }
    if (ec) { err = "遍历目录失败 " + src.string() + ": " + ec.message(); return false; }
    err.clear();
    return true;
}

// 非递归列出某目录下匹配扩展名的文件（排序后返回，保证 classpath 稳定）
inline std::vector<fs::path> listFiles(const fs::path& dir, const std::string& ext = "") {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (de.is_regular_file(ec) &&
            (ext.empty() || endsWith(de.path().filename().string(), ext)))
            out.push_back(de.path());
    }
    std::ranges::sort(out);
    return out;
}

// 递归列出匹配扩展名的文件
inline std::vector<fs::path> recursiveFiles(const fs::path& dir, const std::string& ext = "") {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) &&
            (ext.empty() || endsWith(it->path().filename().string(), ext)))
            out.push_back(it->path());
    }
    std::ranges::sort(out);
    return out;
}

inline std::string readText(const fs::path& p, std::string& err) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { err = "无法读取 " + p.string(); return {}; }
    std::ostringstream ss;
    ss << in.rdbuf();
    err.clear();
    return ss.str();
}
inline bool writeText(const fs::path& p, const std::string& text, std::string& err) {
    if (!makeDirs(p.parent_path(), err)) return false;
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    if (!o) { err = "无法写入 " + p.string(); return false; }
    o.write(text.data(), static_cast<std::streamsize>(text.size()));
    o.flush();
    if (!o) { err = "写文件失败 " + p.string(); return false; }
    err.clear();
    return true;
}

inline bool isWritableDir(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return false;
    return ::access(p.c_str(), W_OK) == 0;
}

inline std::string relativePosix(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    return fs::relative(p, root, ec).generic_string();
}

// 把 argv 拼成可读字符串（仅用于日志，不做 shell 转义）
inline std::string joinForLog(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) out += ' ';
        bool needQuote = s.empty() || s.find_first_of(" \t\"'") != std::string::npos;
        if (!needQuote) out += s;
        else {
            out += '"';
            for (char c : s) { if (c == '"' || c == '\\') out += '\\'; out += c; }
            out += '"';
        }
    }
    return out;
}

} // namespace fsx
} // namespace mc263
