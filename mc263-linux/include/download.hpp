// download.hpp -- 用系统 curl 下载文件 + 自实现 SHA1 校验 + 并发任务池
// 只依赖: C++23 标准库、POSIX、系统 curl CLI（Fedora/openSUSE TW 自带）
#pragma once

#include "fs_util.hpp"
#include "mini_json.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace mc263::dl {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ SHA1 ----
class Sha1 {
public:
    Sha1() { reset(); }
    void reset() {
        h_[0] = 0x67452301; h_[1] = 0xEFCDAB89; h_[2] = 0x98BADCFE;
        h_[3] = 0x10325476; h_[4] = 0xC3D2E1F0;
        total_ = 0; bufLen_ = 0;
    }
    void update(const uint8_t* data, size_t len) {
        total_ += len;
        while (len) {
            size_t take = std::min(len, sizeof(buf_) - bufLen_);
            std::memcpy(buf_ + bufLen_, data, take);
            bufLen_ += take; data += take; len -= take;
            if (bufLen_ == sizeof(buf_)) { transform(buf_); bufLen_ = 0; }
        }
    }
    std::string hex() {
        uint64_t bits = total_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (bufLen_ != 56) update(&zero, 1);
        uint8_t lenBuf[8];
        for (int i = 0; i < 8; ++i) lenBuf[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(lenBuf, 8);
        char out[41];
        for (int i = 0; i < 5; ++i) std::snprintf(out + i * 8, 9, "%08x", h_[i]);
        out[40] = '\0';
        return out;
    }
    static std::string ofFile(const fs::path& p, std::string& err) {
        std::ifstream in(p, std::ios::binary);
        if (!in) { err = "无法读取 " + p.string(); return {}; }
        Sha1 s;
        std::vector<uint8_t> buf(1 << 20);
        while (in) {
            in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
            size_t n = static_cast<size_t>(in.gcount());
            if (n) s.update(buf.data(), n);
        }
        err.clear();
        return s.hex();
    }
private:
    static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }
    void transform(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
    }
    uint32_t h_[5]{};
    uint64_t total_ = 0;
    size_t bufLen_ = 0;
    uint8_t buf_[64]{};
};

inline bool sha1Matches(const fs::path& p, const std::string& expected, std::string& err) {
    if (expected.empty()) { err.clear(); return mc263::fsx::isFile(p); }
    std::string got = Sha1::ofFile(p, err);
    if (!err.empty()) return false;
    if (got.size() != expected.size()) return false;
    for (size_t i = 0; i < got.size(); ++i) {
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(got[i])));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(expected[i])));
        if (a != b) return false;
    }
    return true;
}

// ------------------------------------------------------------- 镜像替换 ----
inline std::string mirrorUrl(const std::string& url, const std::string& mirror) {
    if (mirror.empty() || mirror == "official") return url;
    // 自定义镜像（含离线自测的 mock CDN）：保留官方 URL 的 path，只替换 host
    if (mirror.rfind("http://", 0) == 0 || mirror.rfind("https://", 0) == 0) {
        std::string base = mirror;
        while (!base.empty() && base.back() == '/') base.pop_back();
        size_t scheme = url.find("://");
        if (scheme == std::string::npos) return url;
        size_t path = url.find('/', scheme + 3);
        if (path == std::string::npos) return base;
        return base + url.substr(path);
    }
    if (mirror == "bmclapi") {
        struct Rule { const char* from; const char* to; };
        static const Rule rules[] = {
            {"https://piston-meta.mojang.com/", "https://bmclapi2.bangbang93.com/"},
            {"https://piston-data.mojang.com/", "https://bmclapi2.bangbang93.com/"},
            {"https://launchermeta.mojang.com/", "https://bmclapi2.bangbang93.com/"},
            {"https://libraries.minecraft.net/", "https://bmclapi2.bangbang93.com/maven/"},
            {"https://resources.download.minecraft.net/", "https://bmclapi2.bangbang93.com/assets/"},
        };
        for (const auto& r : rules) {
            std::string f(r.from);
            if (url.rfind(f, 0) == 0) return std::string(r.to) + url.substr(f.size());
        }
    }
    return url;
}

// ------------------------------------------------------------- curl 后端 ----
// 用 fork + execv 调系统 curl，避免 shell 注入；argv 在 fork 前构造好。
inline bool curlGet(const std::string& url, const fs::path& dest, int timeout, std::string& err) {
    std::string t = std::to_string(timeout);
    std::vector<std::string> a = {
        "curl", "-fL", "-sS", "--retry", "2", "--retry-delay", "1",
        "--connect-timeout", "10", "--max-time", t,
        "-A", "mc263-fetch/1.0", "-o", dest.string(), url
    };
    std::vector<char*> av;
    for (auto& s : a) av.push_back(s.data());
    av.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) { err = "fork 失败"; return false; }
    if (pid == 0) {
        ::execvp("curl", av.data());
        ::_exit(127);
    }
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        err = "curl 失败 (exit=" + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1) + "): " + url;
        return false;
    }
    err.clear();
    return true;
}

inline bool fetchText(const std::string& url, const std::string& mirror,
                      const fs::path& cacheDir, int timeout, std::string& out, std::string& err) {
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    fs::path tmp = cacheDir / ("meta-" + std::to_string(::getpid()) + "-" +
                               std::to_string(std::hash<std::string>{}(url)) + ".tmp");
    if (!curlGet(mirrorUrl(url, mirror), tmp, timeout, err)) {
        fs::remove(tmp, ec);
        return false;
    }
    out = mc263::fsx::readText(tmp, err);
    fs::remove(tmp, ec);
    return err.empty();
}

struct Task {
    std::string url;
    fs::path dest;
    std::string sha1;
    uint32_t mode = 0644;
    std::string label;
};

using ProgressCb = std::function<void(size_t done, size_t total, const std::string& label, bool ok, const std::string& err)>;

inline bool fetchOne(const Task& t, const std::string& mirror, int timeout,
                     bool onlyMissing, std::string& err, bool& cached) {
    cached = false;
    if (onlyMissing && mc263::fsx::isFile(t.dest) && sha1Matches(t.dest, t.sha1, err)) {
        cached = true;
        err.clear();
        return true;
    }
    std::error_code ec;
    fs::create_directories(t.dest.parent_path(), ec);
    fs::path part = t.dest;
    part += ".part";
    fs::remove(part, ec);
    if (!curlGet(mirrorUrl(t.url, mirror), part, timeout, err)) {
        fs::remove(part, ec);
        return false;
    }
    if (!sha1Matches(part, t.sha1, err)) {
        fs::remove(part, ec);
        err = "SHA1 校验失败: " + t.dest.string();
        return false;
    }
    fs::rename(part, t.dest, ec);
    if (ec) {
        fs::remove(part, ec);
        err = "重命名失败: " + t.dest.string();
        return false;
    }
    ::chmod(t.dest.c_str(), t.mode);
    err.clear();
    return true;
}

// 并发任务池。返回失败数；allowPartial=false 时任一失败即整体判定失败。
inline size_t runTasks(const std::vector<Task>& tasks, const std::string& mirror,
                       size_t jobs, int timeout, bool onlyMissing,
                       const ProgressCb& cb, std::vector<std::string>& failures) {
    if (tasks.empty()) return 0;
    std::atomic<size_t> next{0}, done{0}, failed{0};
    std::mutex mtx;
    jobs = std::max<size_t>(1, std::min<size_t>(jobs, 16));
    std::vector<std::thread> pool;
    for (size_t k = 0; k < jobs; ++k) {
        pool.emplace_back([&] {
            while (true) {
                size_t i = next.fetch_add(1);
                if (i >= tasks.size()) break;
                const Task& t = tasks[i];
                std::string e;
                bool cached = false;
                bool ok = fetchOne(t, mirror, timeout, onlyMissing, e, cached);
                size_t d = done.fetch_add(1) + 1;
                if (!ok) {
                    failed.fetch_add(1);
                    std::lock_guard<std::mutex> lk(mtx);
                    failures.push_back(t.label + ": " + e);
                }
                if (cb) {
                    std::lock_guard<std::mutex> lk(mtx);
                    cb(d, tasks.size(), t.label, ok, cached ? "cached" : e);
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    return failed.load();
}

} // namespace mc263::dl
