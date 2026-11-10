// safe_path.hpp -- 输出路径守卫（C++17/23，header-only）
//
// 规则（默认，无需配置）：
//   1. 所有写入目标必须位于「沙箱根目录」之下。默认沙箱根 = mc263-linux 项目目录
//      （自动从可执行文件位置/当前目录向上找 Makefile + src/build_263.cpp 推导）。
//   2. 沙箱外的系统敏感路径一律硬拒绝：/、/etc、/usr、/bin、/sbin、/lib*、
//      /boot、/dev、/proc、/sys、/root、/var、/opt、/run、/srv、/mnt、/media、
//      /home（含其子目录，避免把文件散落到用户主目录）。
//   3. 沙箱外的其它路径（如 /tmp、/data）默认也拒绝，除非 MC263_ALLOW_OUTSIDE=1。
//   4. 路径先 weakly_canonical 规范化，能挡住 ".." 穿越与符号链接逃逸。
//
// 环境变量：
//   MC263_SANDBOX_ROOT   覆盖沙箱根目录（例如放到专用数据盘）
//   MC263_ALLOW_OUTSIDE=1 允许沙箱外、非系统敏感路径（需谨慎）
//
// 注意：只校验「写入」目标；只读来源（已有 .minecraft、Java runtime、launcher ELF）
// 不受限制，方便复用官方启动器已下载的文件。
#pragma once

#include "fs_util.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace mc263::safe {

namespace fs = std::filesystem;

inline fs::path normalize(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) return p.lexically_normal();
    fs::path can = fs::weakly_canonical(abs, ec);
    if (ec) return abs.lexically_normal();
    return can.lexically_normal();
}

// p == root 或 p 在 root 之下
inline bool isUnder(const fs::path& p, const fs::path& root) {
    auto pit = p.begin(), rend = root.end();
    for (auto rit = root.begin(); rit != rend; ++rit, ++pit) {
        if (pit == p.end() || *pit != *rit) return false;
    }
    return true;
}

// 系统关键路径：不允许作为沙箱根，也不允许写入（沙箱内例外见 checkWrite）
inline const std::vector<fs::path>& critical() {
    static const std::vector<fs::path> v = {
        "/etc", "/usr", "/bin", "/sbin", "/lib", "/lib64", "/lib32", "/libx32",
        "/boot", "/dev", "/proc", "/sys", "/root", "/var", "/opt", "/run",
        "/srv", "/mnt", "/media", "/selinux",
    };
    return v;
}
inline bool isCritical(const fs::path& abs) {
    if (abs == fs::path("/")) return true;
    for (const auto& b : critical()) if (isUnder(abs, b)) return true;
    return false;
}
// 黑名单 = 系统关键路径 + /home 及其子目录
inline bool isBlocked(const fs::path& abs) {
    return isCritical(abs) || isUnder(abs, fs::path("/home"));
}

inline fs::path defaultSandboxRoot(const char* argv0 = nullptr) {
    std::string env = mc263::fsx::env("MC263_SANDBOX_ROOT");
    if (!env.empty()) return normalize(fs::path(env));
    auto probe = [](fs::path p) -> fs::path {
        for (int i = 0; i < 5 && !p.empty(); ++i, p = p.parent_path()) {
            if (mc263::fsx::isFile(p / "Makefile") &&
                mc263::fsx::isFile(p / "src/build_263.cpp")) return p;
        }
        return {};
    };
    if (argv0) {
        fs::path r = probe(fs::absolute(argv0).parent_path());
        if (!r.empty()) return normalize(r);
    }
    fs::path r = probe(fs::current_path());
    if (!r.empty()) return normalize(r);
    return normalize(fs::current_path());
}

struct Policy {
    fs::path sandboxRoot;
    bool allowOutside = false;
    // 用户显式指定的目录（比如自己选在 ~/Games 下的游戏目录）：也算可信写入区。
    // 系统关键路径（/etc、/usr…）不会被加进来，见 Layout::at()。
    std::vector<fs::path> extraRoots;
};

inline Policy policyFromEnv(const char* argv0 = nullptr) {
    Policy p;
    p.sandboxRoot = defaultSandboxRoot(argv0);
    p.allowOutside = mc263::fsx::env("MC263_ALLOW_OUTSIDE") == "1";
    return p;
}

struct Result {
    bool ok = false;
    std::string reason;
    fs::path resolved;
};

inline Result checkWrite(const fs::path& target, const Policy& pol) {
    fs::path abs = normalize(target);
    fs::path root = normalize(pol.sandboxRoot);
    if (isCritical(root))
        return {false, "沙箱根目录本身是系统敏感路径: " + root.string(), abs};
    if (isUnder(abs, root)) return {true, "", abs};
    for (const auto& extra : pol.extraRoots)
        if (isUnder(abs, normalize(extra))) return {true, "", abs};
    if (isBlocked(abs))
        return {false, "拒绝写入受保护目录: " + abs.string() +
                           "（/、/home、/etc 等系统敏感路径一律禁用）", abs};
    if (!pol.allowOutside)
        return {false, "输出超出沙箱根目录 " + root.string() +
                           "；可设置 MC263_SANDBOX_ROOT 或 MC263_ALLOW_OUTSIDE=1", abs};
    return {true, "", abs};
}

inline std::string describe(const Policy& p) {
    return "沙箱根=" + normalize(p.sandboxRoot).string() +
           (p.allowOutside ? " | 允许沙箱外非敏感路径" : " | 仅限沙箱内");
}

} // namespace mc263::safe
