// downloader-test.cpp -- 离线下载核心测试 CLI（不依赖 Qt）
// 与旧的 Python 下载器 CLI 参数保持近似，便于 mock CDN 复用。
#include "launcher_core.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

struct Args {
    fs::path mcDir;
    std::string version = "26.3";
    std::string mirror = "official";
    std::string components = "jar,libraries,assets,runtime";
    int jobs = 10;
    bool verify = false;
    bool list = false;
    bool latest = false;
    bool snapshots = false;
    size_t limit = 0;
    int cancelAfterMs = 0;
};

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::vector<std::string> splitComps(const std::string& s) {
    std::vector<std::string> out;
    size_t begin = 0;
    while (begin <= s.size()) {
        const size_t end = s.find(',', begin);
        const std::string part = s.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!part.empty()) out.push_back(part);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return out;
}

bool parseArgs(int argc, char** argv, Args& a, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { err = std::string("缺少参数值: ") + name; return nullptr; }
            return argv[++i];
        };
        if (k == "--mc-dir") {
            const char* v = need("--mc-dir");
            if (!v) return false;
            a.mcDir = v;
        } else if (k == "--version") {
            const char* v = need("--version");
            if (!v) return false;
            a.version = v;
        } else if (k == "--mirror") {
            const char* v = need("--mirror");
            if (!v) return false;
            a.mirror = v;
        } else if (k == "--base-url") {
            const char* v = need("--base-url");
            if (!v) return false;
            a.mirror = v;
        } else if (k == "--components") {
            const char* v = need("--components");
            if (!v) return false;
            a.components = v;
        } else if (k == "--jobs") {
            const char* v = need("--jobs");
            if (!v) return false;
            a.jobs = std::max(1, std::atoi(v));
        } else if (k == "--limit") {
            const char* v = need("--limit");
            if (!v) return false;
            a.limit = static_cast<size_t>(std::max(0, std::atoi(v)));
        } else if (k == "--cancel-after-ms") {
            const char* v = need("--cancel-after-ms");
            if (!v) return false;
            a.cancelAfterMs = std::max(0, std::atoi(v));
        } else if (k == "--verify") {
            a.verify = true;
        } else if (k == "--list-versions") {
            a.list = true;
        } else if (k == "--latest-snapshot") {
            a.latest = true;
        } else if (k == "--snapshots") {
            a.snapshots = true;
        } else {
            err = "未知参数: " + k;
            return false;
        }
    }
    if (a.mcDir.empty()) { err = "必须指定 --mc-dir"; return false; }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    std::string err;
    if (!parseArgs(argc, argv, a, err)) {
        std::cerr << err << "\n";
        return 2;
    }

    const fs::path root = a.mcDir.parent_path().empty() ? fs::path(".") : a.mcDir.parent_path();
    mc263::core::Layout l = mc263::core::Layout::at(root, a.mcDir.string());
    if (!l.ensure(err)) {
        std::cerr << "创建目录失败: " << err << "\n";
        return 2;
    }
    mc263::core::Config c;
    c.version = a.version;
    c.mirror = a.mirror;
    c.arch = mc263::core::detectArch();
    c.jobs = a.jobs;
    c.components = splitComps(a.components);

    const fs::path cache = l.configDir / "cache";
    if (a.latest) {
        const std::string id = mc263::core::latestSnapshot(a.mirror, cache, err);
        if (!err.empty()) {
            std::cerr << err << "\n";
            return 2;
        }
        std::cout << "{\"version\":\"" << escape(id) << "\"}\n";
        return 0;
    }
    if (a.list) {
        auto vers = mc263::core::onlineVersions(a.mirror, cache, a.snapshots, 0, err);
        if (!err.empty()) {
            std::cerr << err << "\n";
            return 2;
        }
        std::cout << "{\"versions\":[";
        for (size_t i = 0; i < vers.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << "\"" << escape(vers[i]) << "\"";
        }
        std::cout << "]}\n";
        return 0;
    }

    auto cancel = std::make_shared<std::atomic_bool>(false);
    if (a.cancelAfterMs > 0) {
        std::thread([cancel, ms = a.cancelAfterMs] {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            cancel->store(true);
        }).detach();
    }

    mc263::core::DownloadOptions opt;
    opt.mode = a.verify ? mc263::core::DownloadMode::Verify : mc263::core::DownloadMode::Install;
    opt.limit = a.limit;
    auto progress = [](const mc263::core::DownloadProgress& p) {
        std::cout << "{\"stage\":\"" << escape(p.stage) << "\",\"done\":" << p.done
                  << ",\"total\":" << p.total << ",\"label\":\"" << escape(p.label)
                  << "\",\"ok\":" << (p.ok ? "true" : "false") << ",\"note\":\""
                  << escape(p.note) << "\"}\n";
        std::cout.flush();
    };
    const auto result = mc263::core::downloadAll(
        l, c, progress, opt, [cancel] { return cancel->load(); });

    if (result.cancelled) {
        std::cout << "{\"stage\":\"cancelled\"}\n";
        return 4;
    }
    std::cout << "{\"stage\":\"done\",\"ok\":" << (result.ok ? "true" : "false")
              << ",\"checked\":" << result.checked << ",\"bad\":" << result.bad
              << ",\"downloaded\":" << result.downloaded << ",\"cached\":" << result.cached
              << ",\"failed\":" << result.failed << ",\"error\":\"" << escape(result.error)
              << "\"}\n";
    return result.ok ? 0 : 1;
}
