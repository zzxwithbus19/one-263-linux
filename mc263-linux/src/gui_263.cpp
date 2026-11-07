// gui_263.cpp -- winetricks 风格的图形化配置页（本地 Web UI + zenity 回退）
//
// 默认: 在 127.0.0.1 起一个临时 HTTP 服务，自动用 xdg-open 打开配置页；
//       页面里勾选组件、填版本/镜像/内存/mods URL，一键「下载 → 构建 → 打 rpm」，
//       实时显示进度与日志。配置存到 ~/.config/mc263/config.json。
// --zenity: 无浏览器时用 zenity --checklist / --entry / --progress 完成同样流程。
//
// 安全: 只监听 127.0.0.1、校验 Host、fork+execv 传参数（不拼 shell）。
#include "fs_util.hpp"
#include "mini_json.hpp"
#include "safe_path.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace fsx = mc263::fsx;
using mc263::JValue;

static const char* kUsage = R"(mc263-gui -- winetricks 风格的图形化配置

用法: mc263-gui [选项]

  --no-browser      不自动打开浏览器，只打印 URL
  --port N          指定端口（默认自动分配）
  --page PATH       指定配置页 HTML（默认 gui/index.html）
  --zenity          使用 zenity 对话框模式（--checklist / --entry / --progress）
  --tool-dir PATH   mc263-fetch / mc263-build 所在目录（默认同目录）
  --help            显示本帮助

Web API: GET /api/config | POST /api/config | GET /api/versions |
         POST /api/start | GET /api/status | POST /api/cancel | GET /api/health

路径安全: 配置文件与所有下载/构建输出默认只写项目沙箱内；
         /、/home、/etc、/usr 等一律拒绝（POST /api/config 也会校验）。
         MC263_SANDBOX_ROOT 改沙箱根，MC263_ALLOW_OUTSIDE=1 放行沙箱外非敏感路径。
)";

struct Options {
    bool noBrowser = false, zenity = false, help = false;
    int port = 0;
    std::string page, toolDir;
};
static Options parseArgs(int argc, char** argv) {
    Options o;
    o.toolDir = fs::absolute(argv[0]).parent_path().string();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--help" || a == "-h") o.help = true;
        else if (a == "--no-browser") o.noBrowser = true;
        else if (a == "--zenity") o.zenity = true;
        else if (a == "--port") o.port = std::atoi(val().c_str());
        else if (a == "--page") o.page = val();
        else if (a == "--tool-dir") o.toolDir = val();
        else std::println(stderr, "警告: 忽略未知参数 {}", a);
    }
    if (o.page.empty()) {
        for (const fs::path& p : {fs::path("gui/index.html"),
                                  fs::path(o.toolDir) / "../gui/index.html",
                                  fs::path(o.toolDir) / "gui/index.html"}) {
            if (fsx::isFile(p)) { o.page = p.string(); break; }
        }
    }
    return o;
}

// ------------------------------------------------------------- 配置 --------
// 配置也只写项目沙箱内，不再落到 ~/.config
static fs::path configPath() {
    return mc263::safe::defaultSandboxRoot() / ".mc263/config.json";
}

static JValue defaultConfig() {
    JValue c = JValue::objectV();
    fs::path root = mc263::safe::defaultSandboxRoot();
    c.set("version", JValue::stringV("26.3"));
    c.set("mcDir", JValue::stringV((root / "gamefiles").string()));
    c.set("output", JValue::stringV((root / "dist/mc263").string()));
    c.set("mirror", JValue::stringV("official"));
    c.set("arch", JValue::stringV("x86_64"));
    c.set("username", JValue::stringV(""));
    c.set("memory", JValue::stringV("4G"));
    c.set("jobs", JValue::numberV(8));
    c.set("rpm", JValue::booleanV(true));
    c.set("fabric", JValue::booleanV(false));
    JValue comps = JValue::arrayV();
    for (const char* s : {"jar", "libraries", "assets", "runtime"}) comps.push(JValue::stringV(s));
    c.set("components", comps);
    c.set("modUrls", JValue::arrayV());
    return c;
}
static JValue loadConfig() {
    std::string err;
    std::string text = fsx::readText(configPath(), err);
    if (!err.empty()) return defaultConfig();
    JValue c = mc263::jsonParse(text, err);
    if (!err.empty() || !c.isObject()) return defaultConfig();
    JValue def = defaultConfig();
    for (const auto& kv : def.object()) if (!c.find(kv.first)) c.set(kv.first, kv.second);
    return c;
}
static bool saveConfig(const JValue& c, std::string& err) {
    fs::path p = configPath();
    return fsx::writeText(p, mc263::jsonDump(c), err);
}

// 所有会被写入的路径都要过守卫（配置来自 HTTP / zenity，必须视为不可信输入）
static bool validateConfig(const JValue& c, std::string& err) {
    auto pol = mc263::safe::policyFromEnv();
    auto cs = [&](const char* k, const std::string& d) {
        const JValue* v = c.find(k);
        return (v && v->isString()) ? v->str() : d;
    };
    fs::path root = mc263::safe::defaultSandboxRoot();
    auto r1 = mc263::safe::checkWrite(fs::path(cs("mcDir", (root / "gamefiles").string())), pol);
    auto r2 = mc263::safe::checkWrite(fs::path(cs("output", (root / "dist/mc263").string())), pol);
    auto r3 = mc263::safe::checkWrite(configPath(), pol);
    if (!r1.ok) { err = "游戏文件目录 " + r1.reason; return false; }
    if (!r2.ok) { err = "输出单文件 " + r2.reason; return false; }
    if (!r3.ok) { err = "配置文件 " + r3.reason; return false; }
    err.clear();
    return true;
}

// ------------------------------------------------------------- 进程 --------
struct Status {
    std::mutex mtx;
    bool running = false, finished = false, ok = false;
    std::string phase = "空闲", message = "";
    size_t done = 0, total = 0;
    std::deque<std::string> logs;
    std::atomic<bool> cancel{false};
    pid_t child = -1;
    void log(const std::string& s) {
        std::lock_guard<std::mutex> lk(mtx);
        logs.push_back(s);
        while (logs.size() > 400) logs.pop_front();
    }
    void set(const std::string& ph, const std::string& msg = "") {
        std::lock_guard<std::mutex> lk(mtx);
        phase = ph; message = msg;
    }
    JValue json() {
        std::lock_guard<std::mutex> lk(mtx);
        JValue j = JValue::objectV();
        j.set("running", JValue::booleanV(running));
        j.set("finished", JValue::booleanV(finished));
        j.set("ok", JValue::booleanV(ok));
        j.set("phase", JValue::stringV(phase));
        j.set("message", JValue::stringV(message));
        j.set("done", JValue::numberV(static_cast<double>(done)));
        j.set("total", JValue::numberV(static_cast<double>(total)));
        JValue arr = JValue::arrayV();
        size_t start = logs.size() > 120 ? logs.size() - 120 : 0;
        for (size_t i = start; i < logs.size(); ++i) arr.push(JValue::stringV(logs[i]));
        j.set("logs", arr);
        return j;
    }
};
static Status g_status;

// fork+execv 捕获 stdout/stderr，逐行回调；支持取消
static int runCapture(const std::vector<std::string>& argv,
                      const std::function<void(const std::string&)>& onLine) {
    int fds[2];
    if (::pipe(fds) != 0) return -1;
    std::vector<std::string> args = argv;
    std::vector<char*> av;
    for (auto& s : args) av.push_back(s.data());
    av.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) { ::close(fds[0]); ::close(fds[1]); return -1; }
    if (pid == 0) {
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[0]); ::close(fds[1]);
        ::execvp(argv[0].c_str(), av.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    {
        std::lock_guard<std::mutex> lk(g_status.mtx);
        g_status.child = pid;
    }
    FILE* f = ::fdopen(fds[0], "r");
    char* line = nullptr;
    size_t cap = 0;
    ssize_t n;
    while ((n = ::getline(&line, &cap, f)) != -1) {
        std::string s(line, static_cast<size_t>(n));
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (g_status.cancel.load()) { ::kill(pid, SIGTERM); break; }
        if (onLine) onLine(s);
        g_status.log(s);
    }
    if (line) std::free(line);
    ::fclose(f);
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    {
        std::lock_guard<std::mutex> lk(g_status.mtx);
        g_status.child = -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// ------------------------------------------------------------- 任务 --------
static void runTask(const Options& o) {
    {
        std::lock_guard<std::mutex> lk(g_status.mtx);
        g_status.running = true; g_status.finished = false; g_status.ok = false;
        g_status.done = 0; g_status.total = 0; g_status.logs.clear();
    }
    g_status.cancel.store(false);
    JValue c = loadConfig();
    std::string pathErr;
    if (!validateConfig(c, pathErr)) {
        g_status.set("路径被拒绝", pathErr);
        g_status.log("安全策略: " + pathErr);
        std::lock_guard<std::mutex> lk(g_status.mtx);
        g_status.running = false; g_status.finished = true; g_status.ok = false;
        return;
    }
    auto cs = [&](const char* k, const std::string& d) {
        const JValue* v = c.find(k);
        return (v && v->isString()) ? v->str() : d;
    };
    std::string version = cs("version", "26.3");
    std::string mcDir = cs("mcDir", fsx::home() + "/.local/share/mc263/gamefiles");
    std::string output = cs("output", fsx::home() + "/mc263");
    std::string mirror = cs("mirror", "official");
    std::string arch = cs("arch", "x86_64");
    std::string memory = cs("memory", "4G");
    size_t jobs = static_cast<size_t>(c.find("jobs") ? c.find("jobs")->number(8) : 8);
    bool wantRpm = c.find("rpm") && c.find("rpm")->boolean();
    bool wantFabric = c.find("fabric") && c.find("fabric")->boolean();

    // 1) 下载
    g_status.set("下载游戏文件", "mc263-fetch");
    std::vector<std::string> fargv = {
        (fs::path(o.toolDir) / "mc263-fetch").string(),
        "--mc-dir", mcDir, "--version", version,
        "--mirror", mirror, "--arch", arch,
        "--jobs", std::to_string(jobs), "--json-progress"
    };
    const JValue* comps = c.find("components");
    size_t compCount = 0;
    if (comps && comps->isArray()) {
        for (const JValue& v : comps->array()) if (v.isString()) {
            fargv.push_back("--component"); fargv.push_back(v.str());
            ++compCount;
        }
    }
    if (wantFabric) { fargv.push_back("--component"); fargv.push_back("fabric"); ++compCount; }
    if (compCount == 0) { fargv.push_back("--component"); fargv.push_back("none"); }
    const JValue* urls = c.find("modUrls");
    if (urls && urls->isArray()) {
        for (const JValue& v : urls->array()) if (v.isString() && !v.str().empty()) {
            fargv.push_back("--mod-url"); fargv.push_back(v.str());
        }
    }
    // 显式组件为空时至少保证 version json
    auto onFetchLine = [](const std::string& line) {
        if (line.rfind("{", 0) != 0) return;
        std::string err;
        JValue j = mc263::jsonParse(line, err);
        if (!err.empty()) return;
        const JValue* st = j.find("stage");
        const JValue* d = j.find("done");
        const JValue* t = j.find("total");
        const JValue* lab = j.find("label");
        std::lock_guard<std::mutex> lk(g_status.mtx);
        if (st) g_status.phase = "下载: " + st->str();
        if (d) g_status.done = static_cast<size_t>(d->number());
        if (t) g_status.total = static_cast<size_t>(t->number());
        if (lab) g_status.message = lab->str();
    };
    int rc = runCapture(fargv, onFetchLine);
    if (rc != 0) {
        g_status.set("下载失败", "mc263-fetch 退出码 " + std::to_string(rc));
        std::lock_guard<std::mutex> lk(g_status.mtx);
        g_status.running = false; g_status.finished = true; g_status.ok = false;
        return;
    }
    if (g_status.cancel.load()) {
        g_status.set("已取消", "用户取消");
        std::lock_guard<std::mutex> lk(g_status.mtx);
        g_status.running = false; g_status.finished = true;
        return;
    }

    // 2) 构建
    g_status.set("构建单文件", "mc263-build");
    std::string buildVersion = wantFabric ? version + "+fabric" : version;
    std::vector<std::string> bargv = {
        (fs::path(o.toolDir) / "mc263-build").string(),
        "--mc-dir", mcDir, "--version", buildVersion,
        "--arch", arch, "--output", output,
        "--launcher", (fs::path(o.toolDir) / "launcher").string(),
        "--work-dir", (fs::path(fsx::home()) / ".cache/mc263/gui-stage").string(),
        "--extra-jvm-arg", "-Xmx" + memory
    };
    const JValue* user = c.find("username");
    if (user && !user->str().empty()) bargv.insert(bargv.end(), {"--username", user->str()});
    rc = runCapture(bargv, nullptr);
    if (rc != 0) {
        g_status.set("构建失败", "mc263-build 退出码 " + std::to_string(rc));
        std::lock_guard<std::mutex> lk(g_status.mtx);
        g_status.running = false; g_status.finished = true; g_status.ok = false;
        return;
    }

    // 3) rpm
    if (wantRpm) {
        g_status.set("打包 rpm", "build-rpm.sh");
        std::string script = (fs::path(o.toolDir).parent_path() / "packaging/build-rpm.sh").string();
        if (!fsx::isFile(fs::path(script))) script = "packaging/build-rpm.sh";
        std::vector<std::string> rargv = {"bash", script, "--payload", output,
                                          "--out", (fs::path(output).parent_path()).string()};
        rc = runCapture(rargv, nullptr);
        if (rc != 0) g_status.log("警告: rpm 打包失败（单文件本身已生成）");
    }
    {
        std::lock_guard<std::mutex> lk(g_status.mtx);
        g_status.running = false; g_status.finished = true; g_status.ok = true;
        g_status.phase = "完成";
        g_status.message = output;
        g_status.done = g_status.total;
    }
}

// ------------------------------------------------------------- HTTP --------
static void send(int fd, int code, const std::string& type, const std::string& body) {
    std::string head = "HTTP/1.1 " + std::to_string(code) + " OK\r\n"
                       "Content-Type: " + type + "; charset=utf-8\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n"
                       "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    ::send(fd, head.data(), head.size(), MSG_NOSIGNAL);
    size_t off = 0;
    while (off < body.size()) {
        ssize_t n = ::send(fd, body.data() + off, body.size() - off, MSG_NOSIGNAL);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}
static void sendJson(int fd, const JValue& j) {
    std::string body = mc263::jsonDump(j);
    send(fd, 200, "application/json", body);
}

static std::string pageHtml(const Options& o) {
    std::string err;
    std::string html = fsx::readText(fs::path(o.page), err);
    if (!err.empty()) {
        return "<!doctype html><meta charset='utf-8'><h1>找不到配置页</h1><p>" +
               o.page + "</p><p>请用 --page 指定 gui/index.html</p>";
    }
    return html;
}

static std::vector<std::string> fetchVersionList(const Options& o, std::string& err) {
    std::vector<std::string> out;
    std::vector<std::string> argv = {(fs::path(o.toolDir) / "mc263-fetch").string(), "--list-versions"};
    int rc = runCapture(argv, [&](const std::string& line) {
        std::string s = trim(line);
        if (!s.empty() && s.find(' ') == std::string::npos && s[0] >= '0' && s[0] <= '9') out.push_back(s);
    });
    if (rc != 0) err = "获取版本列表失败（可手动填写版本号）";
    if (out.empty()) out = {"26.3", "26.2", "26.1.2"};
    return out;
}

static void handle(int fd, const Options& o) {
    char buf[65536];
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return;
    std::string req(buf, static_cast<size_t>(n));
    size_t lineEnd = req.find("\r\n");
    std::string reqLine = lineEnd == std::string::npos ? req : req.substr(0, lineEnd);
    std::string method, path;
    {
        size_t p1 = reqLine.find(' '), p2 = reqLine.find(' ', p1 == std::string::npos ? 0 : p1 + 1);
        if (p1 != std::string::npos) {
            method = reqLine.substr(0, p1);
            path = reqLine.substr(p1 + 1, p2 == std::string::npos ? p2 : p2 - p1 - 1);
        }
    }
    // Host 校验，避免 DNS rebinding
    std::string host;
    {
        std::string lower = req;
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        size_t hp = lower.find("\r\nhost:");
        if (hp != std::string::npos) {
            size_t e = lower.find("\r\n", hp + 2);
            host = lower.substr(hp + 7, e == std::string::npos ? e : e - hp - 7);
            host = trim(host);
        }
    }
    if (!host.empty() && host.find("127.0.0.1") == std::string::npos &&
        host.find("localhost") == std::string::npos) {
        send(fd, 403, "text/plain", "forbidden");
        return;
    }
    // body
    std::string body;
    size_t bodyPos = req.find("\r\n\r\n");
    if (bodyPos != std::string::npos) body = req.substr(bodyPos + 4);
    size_t contentLen = 0;
    {
        std::string lower = req;
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        size_t cl = lower.find("\r\ncontent-length:");
        if (cl != std::string::npos) {
            size_t e = lower.find("\r\n", cl + 2);
            contentLen = static_cast<size_t>(std::atoll(lower.substr(cl + 17, e - cl - 17).c_str()));
        }
    }
    while (body.size() < contentLen) {
        ssize_t got = ::recv(fd, buf, sizeof(buf), 0);
        if (got <= 0) break;
        body.append(buf, static_cast<size_t>(got));
    }

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        send(fd, 200, "text/html", pageHtml(o));
    } else if (method == "GET" && path == "/api/health") {
        JValue j = JValue::objectV(); j.set("ok", JValue::booleanV(true));
        sendJson(fd, j);
    } else if (method == "GET" && path == "/api/config") {
        sendJson(fd, loadConfig());
    } else if (method == "POST" && path == "/api/config") {
        std::string err;
        JValue c = mc263::jsonParse(body, err);
        if (!err.empty() || !c.isObject()) { send(fd, 400, "text/plain", "invalid json"); return; }
        std::string pathErr;
        if (!validateConfig(c, pathErr)) {
            JValue bad = JValue::objectV();
            bad.set("ok", JValue::booleanV(false));
            bad.set("error", JValue::stringV(pathErr));
            sendJson(fd, bad);
            return;
        }
        bool ok = saveConfig(c, err);
        JValue j = JValue::objectV();
        j.set("ok", JValue::booleanV(ok));
        j.set("error", JValue::stringV(err));
        sendJson(fd, j);
    } else if (method == "GET" && path == "/api/versions") {
        std::string err;
        JValue j = JValue::objectV();
        JValue arr = JValue::arrayV();
        for (const auto& v : fetchVersionList(o, err)) arr.push(JValue::stringV(v));
        j.set("versions", arr);
        j.set("error", JValue::stringV(err));
        sendJson(fd, j);
    } else if (method == "POST" && path == "/api/start") {
        bool running;
        {
            std::lock_guard<std::mutex> lk(g_status.mtx);
            running = g_status.running;
        }
        JValue j = JValue::objectV();
        if (running) { j.set("ok", JValue::booleanV(false)); j.set("error", JValue::stringV("任务已在运行")); sendJson(fd, j); return; }
        std::thread(&runTask, o).detach();
        j.set("ok", JValue::booleanV(true));
        sendJson(fd, j);
    } else if (method == "GET" && path == "/api/status") {
        sendJson(fd, g_status.json());
    } else if (method == "POST" && path == "/api/cancel") {
        g_status.cancel.store(true);
        pid_t child;
        {
            std::lock_guard<std::mutex> lk(g_status.mtx);
            child = g_status.child;
        }
        if (child > 0) ::kill(child, SIGTERM);
        JValue j = JValue::objectV(); j.set("ok", JValue::booleanV(true));
        sendJson(fd, j);
    } else {
        send(fd, 404, "text/plain", "not found");
    }
}

// ------------------------------------------------------------- zenity ------
static std::string zenityAsk(const std::vector<std::string>& argv, int& rc) {
    std::string out;
    rc = runCapture(argv, [&](const std::string& line) { out += line; });
    return trim(out);
}

static int zenityFlow(const Options& o) {
    JValue c = loadConfig();
    std::vector<std::string> checklist = {
        "zenity", "--list", "--checklist", "--width", "640", "--height", "360",
        "--title", "mc263 · 选择组件", "--text", "类似 winetricks：勾选要下载并打包的组件",
        "--column", "选", "--column", "组件", "--column", "说明",
        "TRUE", "jar", "游戏主程序 client.jar",
        "TRUE", "libraries", "依赖库 + natives-linux (.so)",
        "TRUE", "assets", "资源文件（约 485 MB）",
        "TRUE", "runtime", "官方 Java 25 运行时",
        "FALSE", "fabric", "Fabric Loader（实验性）",
        "FALSE", "mods", "额外 mods URL"
    };
    int rc = 0;
    std::string picked = zenityAsk(checklist, rc);
    if (rc != 0 || picked.empty()) { std::println("已取消"); return 1; }
    std::vector<std::string> parts;
    size_t st = 0;
    while (true) {
        size_t q = picked.find('|', st);
        parts.push_back(picked.substr(st, q == std::string::npos ? q : q - st));
        if (q == std::string::npos) break;
        st = q + 1;
    }
    std::string version = zenityAsk({"zenity", "--entry", "--title", "mc263 · 版本",
                                     "--text", "Minecraft 版本:", "--entry-text", "26.3"}, rc);
    if (version.empty()) version = "26.3";
    std::string mirror = zenityAsk({"zenity", "--list", "--radiolist", "--title", "mc263 · 下载源",
                                    "--text", "选择镜像", "--column", "选", "--column", "源",
                                    "TRUE", "official", "FALSE", "bmclapi"}, rc);
    if (mirror.empty()) mirror = "official";

    JValue comps = JValue::arrayV();
    bool fabric = false;
    for (const std::string& p : parts) {
        if (p == "jar" || p == "libraries" || p == "assets" || p == "runtime" || p == "mods")
            comps.push(JValue::stringV(p));
        if (p == "fabric") fabric = true;
    }
    c.set("components", comps);
    c.set("fabric", JValue::booleanV(fabric));
    c.set("version", JValue::stringV(version));
    c.set("mirror", JValue::stringV(mirror));
    std::string err;
    if (!validateConfig(c, err)) {
        std::println(stderr, "错误: {}", err);
        zenityAsk({"zenity", "--error", "--title", "mc263", "--text", "路径被拒绝: " + err}, rc);
        return 1;
    }
    saveConfig(c, err);

    // pulsate 进度窗（后台），任务结束后关闭
    std::vector<std::string> pargv = {"zenity", "--progress", "--pulsate", "--auto-close",
                                      "--title", "mc263", "--text", "正在下载并构建单文件..."};
    std::vector<char*> pav;
    for (auto& s : pargv) pav.push_back(s.data());
    pav.push_back(nullptr);
    pid_t zpid = ::fork();
    if (zpid == 0) {
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) { ::dup2(devnull, STDIN_FILENO); ::close(devnull); }
        ::execvp("zenity", pav.data());
        ::_exit(127);
    }
    runTask(o);
    if (zpid > 0) { ::kill(zpid, SIGTERM); int st = 0; ::waitpid(zpid, &st, 0); }
    bool ok, finished;
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(g_status.mtx);
        ok = g_status.ok; finished = g_status.finished; msg = g_status.message;
    }
    zenityAsk({"zenity", (ok ? "--info" : "--error"), "--title", "mc263",
               "--text", (ok ? "完成: " + msg : "失败，请查看日志 " + msg)}, rc);
    return (finished && ok) ? 0 : 1;
}

int main(int argc, char** argv) {
    Options o = parseArgs(argc, argv);
    if (o.help) { std::print("{}", kUsage); return 0; }
    if (o.zenity) return zenityFlow(o);

    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { std::println(stderr, "错误: 无法创建 socket"); return 1; }
    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = ::htons(static_cast<uint16_t>(o.port));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(srv, 16) != 0) {
        std::println(stderr, "错误: 无法监听 127.0.0.1:{}", o.port);
        return 1;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &len);
    int port = ::ntohs(addr.sin_port);
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    std::println("mc263 图形化配置页: {}", url);
    std::println("配置: {} | 工具目录: {}", configPath().string(), o.toolDir);
    std::println("路径策略: {}", mc263::safe::describe(mc263::safe::policyFromEnv()));
    std::println("按 Ctrl+C 退出；任务状态也可用 curl {}/api/status 查看", url);
    if (!o.noBrowser) {
        pid_t pid = ::fork();
        if (pid == 0) {
            std::vector<std::string> a = {"xdg-open", url};
            std::vector<char*> av;
            for (auto& s : a) av.push_back(s.data());
            av.push_back(nullptr);
            ::execvp("xdg-open", av.data());
            ::_exit(0);
        }
    }
    while (true) {
        int fd = ::accept(srv, nullptr, nullptr);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        handle(fd, o);
        ::close(fd);
    }
    ::close(srv);
    return 0;
}
