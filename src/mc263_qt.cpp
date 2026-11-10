// mc263_qt.cpp -- HMCL 式 Qt6 原生启动器（单个 ELF）
//
// 形态：[mc263-qt ELF][appDir/minecraft/ 游戏目录][appDir/.mc263/ 配置与内嵌脚本]
// 下载：内嵌 Python3 脚本（ThreadPoolExecutor 多线程 + SHA1 + JSON 进度），QProcess 异步调用；
// Java 探测与兼容性查验：std::thread 后台执行，QMetaObject::invokeMethod 回主线程刷 UI；
// 启动：QProcess 异步 exec java，主线程显示日志。全程不阻塞界面。
//
// 构建：make USE_QT6=1（只链接 Qt6Widgets/Qt6Gui/Qt6Core，不需要 moc / Q_OBJECT）
//
// 显示后端：启动时按会话自动选择 Wayland / XWayland（见 core::choosePlatform），
// 显式 QT_QPA_PLATFORM=... 或 -platform ... 始终优先；「关于」页会显示当前实际后端。
#include "launcher_core.hpp"
#include "modrinth.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QDesktopServices>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLibraryInfo>
#include <csignal>
#include <execinfo.h>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMainWindow>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QVariantAnimation>
#include <QPushButton>
#include <QSizePolicy>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTabBar>
#include <QTableWidgetItem>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace core = mc263::core;

// ---------------------------------------------------------------- 内嵌脚本 ----
static const char* kDownloaderPy = R"PY(#!/usr/bin/env python3
"""mc263-qt 内嵌下载器：多线程 + SHA1 校验 + JSON 进度（由 Qt ELF 释放到 .mc263/）"""
import argparse
import hashlib
import json
import os
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

MANIFEST = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
RUNTIME_ALL = "https://launchermeta.mojang.com/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json"
ASSET_CDN = "https://resources.download.minecraft.net"

MIRRORS = {
    "https://piston-meta.mojang.com/": "https://bmclapi2.bangbang93.com/",
    "https://piston-data.mojang.com/": "https://bmclapi2.bangbang93.com/",
    "https://launchermeta.mojang.com/": "https://bmclapi2.bangbang93.com/",
    "https://libraries.minecraft.net/": "https://bmclapi2.bangbang93.com/maven/",
    "https://resources.download.minecraft.net/": "https://bmclapi2.bangbang93.com/assets/",
}


def emit(stage, done, total, label, ok=True, note=""):
    print(json.dumps({"stage": stage, "done": done, "total": total,
                      "label": label, "ok": ok, "note": note},
                     ensure_ascii=False), flush=True)


def mirror_url(url, mirror, base):
    if base:
        i = url.find("://")
        p = url.find("/", i + 3) if i >= 0 else -1
        return base.rstrip("/") + (url[p:] if p >= 0 else "")
    if mirror == "bmclapi":
        for a, b in MIRRORS.items():
            if url.startswith(a):
                return b + url[len(a):]
    return url


def get(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "mc263-qt/1.0"})
    # 回环地址（离线自测 / 自建镜像）绕过系统代理，否则代理会返回 502
    if "127.0.0.1" in url or "localhost" in url:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    else:
        opener = urllib.request.build_opener()
    with opener.open(req, timeout=timeout) as r:
        return r.read()


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def fetch_one(task):
    dest = task["dest"]
    want = task.get("sha1", "")
    if want and os.path.isfile(dest):
        with open(dest, "rb") as f:
            if sha1(f.read()) == want:
                return "cached", task
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    data = get(task["url"], 180)
    if want and sha1(data) != want:
        raise ValueError("SHA1 校验失败")
    tmp = dest + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, dest)
    os.chmod(dest, task.get("mode", 0o644))
    return "ok", task


def run_stage(stage, tasks, jobs):
    if not tasks:
        return 0
    total = len(tasks)
    done = failed = 0
    with ThreadPoolExecutor(max_workers=max(1, min(16, jobs))) as pool:
        futures = {pool.submit(fetch_one, t): t for t in tasks}
        for fut in as_completed(futures):
            done += 1
            task = futures[fut]
            try:
                state, _ = fut.result()
                emit(stage, done, total, task["label"], True, "cached" if state == "cached" else "")
            except Exception as exc:
                failed += 1
                emit(stage, done, total, task["label"], False, str(exc))
    return failed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mc-dir", required=True)
    ap.add_argument("--version", default="26.3")
    ap.add_argument("--mirror", default="official")
    ap.add_argument("--jobs", type=int, default=10)
    ap.add_argument("--arch", default="x86_64")
    ap.add_argument("--components", default="jar,libraries,assets,runtime")
    ap.add_argument("--base-url", default="")
    ap.add_argument("--list-versions", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()
    comps = set(x for x in args.components.split(",") if x)
    if args.base_url:
        os.environ["no_proxy"] = "127.0.0.1,localhost"
        os.environ["NO_PROXY"] = "127.0.0.1,localhost"

    try:
        mtext = get(mirror_url(MANIFEST, args.mirror, args.base_url))
        manifest = json.loads(mtext)
        if args.list_versions:
            rel = [v["id"] for v in manifest.get("versions", []) if v.get("type") == "release"][:20]
            print(json.dumps({"versions": rel}, ensure_ascii=False), flush=True)
            return 0
        wanted = args.version
        if wanted == "latest":
            wanted = manifest.get("latest", {}).get("release", "26.3")
        entry = next((v for v in manifest.get("versions", []) if v.get("id") == wanted), None)
        if not entry:
            emit("error", 0, 0, "找不到版本 " + wanted, False, "")
            return 2
        vdir = os.path.join(args.mc_dir, "versions", wanted)
        vjson = os.path.join(vdir, wanted + ".json")
        failed = run_stage("version", [{
            "url": mirror_url(entry["url"], args.mirror, args.base_url),
            "dest": vjson, "sha1": entry.get("sha1", ""), "label": wanted + ".json"}], 1)
        if failed:
            return 2
        with open(vjson, "r", encoding="utf-8") as f:
            vj = json.load(f)
        aid = vj.get("assetIndex", {})
        asset_id = aid.get("id", wanted)
        tasks = []
        if "jar" in comps and "client" in vj.get("downloads", {}):
            c = vj["downloads"]["client"]
            tasks.append({"url": mirror_url(c["url"], args.mirror, args.base_url),
                          "dest": os.path.join(vdir, wanted + ".jar"),
                          "sha1": c.get("sha1", ""), "label": wanted + ".jar"})
        if "libraries" in comps:
            keys = (["natives-linux-arm64", "natives-linux-aarch64", "natives-linux"]
                    if args.arch == "aarch64" else ["natives-linux", "natives-linux-x86_64"])
            for lib in vj.get("libraries", []):
                rules = lib.get("rules", [])
                if rules:
                    allowed = False
                    for r in rules:
                        osn = r.get("os", {}).get("name", "")
                        match = not osn or osn in ("linux", "unix", "unknown")
                        if "features" in r:
                            match = False
                        if match:
                            allowed = r.get("action", "allow") == "allow"
                    if not allowed:
                        continue
                d = lib.get("downloads", {})
                arts = []
                if "artifact" in d:
                    arts.append(d["artifact"])
                cls = d.get("classifiers", {})
                for k in keys:
                    if k in cls:
                        arts.append(cls[k])
                        break
                for a in arts:
                    if not a.get("path") or not a.get("url"):
                        continue
                    tasks.append({"url": mirror_url(a["url"], args.mirror, args.base_url),
                                  "dest": os.path.join(args.mc_dir, "libraries", a["path"]),
                                  "sha1": a.get("sha1", ""), "label": a["path"]})
        failed += run_stage("libraries", tasks, args.jobs)

        if "assets" in comps and aid.get("url"):
            ipath = os.path.join(args.mc_dir, "assets", "indexes", asset_id + ".json")
            failed += run_stage("asset-index", [{
                "url": mirror_url(aid["url"], args.mirror, args.base_url),
                "dest": ipath, "sha1": aid.get("sha1", ""), "label": asset_id + ".json"}], 1)
            with open(ipath, "r", encoding="utf-8") as f:
                idx = json.load(f)
            objs = []
            for name, meta in idx.get("objects", {}).items():
                h = meta.get("hash", "")
                if len(h) < 3:
                    continue
                objs.append({"url": mirror_url(ASSET_CDN + "/" + h[:2] + "/" + h,
                                               args.mirror, args.base_url),
                             "dest": os.path.join(args.mc_dir, "assets", "objects", h[:2], h),
                             "sha1": h, "label": name})
            if args.limit:
                objs = objs[:args.limit]
            failed += run_stage("assets", objs, args.jobs)

        if "runtime" in comps and args.arch != "aarch64":
            component = vj.get("javaVersion", {}).get("component", "java-runtime-epsilon")
            allj = json.loads(get(mirror_url(RUNTIME_ALL, args.mirror, args.base_url)))
            entries = allj.get("linux", {}).get(component, [])
            if entries:
                mf = json.loads(get(mirror_url(entries[0]["manifest"]["url"],
                                               args.mirror, args.base_url)))
                rt_root = os.path.join(args.mc_dir, "runtime", component)
                rtasks = []
                for rel, meta in mf.get("files", {}).items():
                    t = meta.get("type", "file")
                    if t == "directory":
                        os.makedirs(os.path.join(rt_root, rel), exist_ok=True)
                        continue
                    raw = meta.get("downloads", {}).get("raw")
                    if not raw:
                        continue
                    rtasks.append({"url": mirror_url(raw["url"], args.mirror, args.base_url),
                                   "dest": os.path.join(rt_root, rel),
                                   "sha1": raw.get("sha1", ""),
                                   "mode": 0o755 if meta.get("executable") else 0o644,
                                   "label": rel})
                if args.limit:
                    rtasks = rtasks[:args.limit]
                failed += run_stage("runtime", rtasks, args.jobs)

        print(json.dumps({"stage": "done", "ok": failed == 0, "failed": failed},
                         ensure_ascii=False), flush=True)
        return 0 if failed == 0 else 1
    except Exception as exc:
        emit("error", 0, 0, "下载失败", False, str(exc))
        return 3


if __name__ == "__main__":
    sys.exit(main())
)PY";

// ------------------------------------------------------------ 显示后端选择 ----
static const char* kRepoUrl = "https://github.com/zzxwithbus19/one-263-linux";

namespace {

// Qt 平台插件目录：QLibraryInfo 的默认目录 + QT_PLUGIN_PATH 追加项
QStringList platformPluginDirs() {
    QStringList dirs;
    const QString base = QLibraryInfo::path(QLibraryInfo::PluginsPath);
    if (!base.isEmpty()) dirs << base + "/platforms";
    const QString extra = qEnvironmentVariable("QT_PLUGIN_PATH");
    for (const QString& p : extra.split(':', Qt::SkipEmptyParts)) {
        const QString d = p.trimmed();
        if (!d.isEmpty()) dirs << d + "/platforms";
    }
    return dirs;
}

// qtwayland 会产出 libqwayland-egl.so / libqwayland-generic.so 等多个文件，
// 统一按前缀判断；xcb 只有一个 libqxcb.so。
bool hasPlatformPlugin(const QStringList& dirs, const QString& filePrefix) {
    for (const QString& d : dirs) {
        QDir dir(d);
        if (!dir.exists()) continue;
        if (!dir.entryList({filePrefix + "*"}, QDir::Files).isEmpty()) return true;
    }
    return false;
}

bool waylandSession() {
    if (!qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()) return true;
    // 会话声明是 wayland 又没有 X11 显示：只有在没有 XWayland 时才需要这样兜底
    return qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland" &&
           qEnvironmentVariable("DISPLAY").isEmpty();
}

// 在 QApplication 构造前定好平台插件；fatal 时由 main 打印说明并退出
core::PlatformPick preparePlatform() {
    const QStringList dirs = platformPluginDirs();
    core::PlatformPick pick = core::choosePlatform(
        qEnvironmentVariable("QT_QPA_PLATFORM").toStdString(), waylandSession(),
        hasPlatformPlugin(dirs, "libqwayland"), hasPlatformPlugin(dirs, "libqxcb"));
    if (!pick.plugin.empty()) qputenv("QT_QPA_PLATFORM", QByteArray::fromStdString(pick.plugin));
    return pick;
}

} // namespace

// ------------------------------------------------------------ 内存占用条 ----
static QString fmtMem(uint64_t mb) {
    if (mb >= 1024) return QString::number(mb / 1024.0, 'f', 1) + " GB";
    return QString::number(mb) + " MB";
}

// 配色（深浅色主题各一套），条和图例共用，避免两处颜色漂移
struct MemColors {
    QColor used;   // 其他程序占用
    QColor cache;  // 缓存（可回收）
    QColor game;   // 分给游戏的 -Xmx
    QColor track;  // 余下可用
    QColor mark;   // -Xmx 右边界标记
};

static MemColors memColors(const QPalette& pal) {
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    if (dark)
        return {QColor("#4f9bff"), QColor("#3c74ad"), QColor("#7a828e"),
                QColor("#2a2c30"), QColor("#fbbf24")};
    return {QColor("#2563eb"), QColor("#8ab4fb"), QColor("#c2c8d2"),
            QColor("#eceef1"), QColor("#b45309")};
}

// 窗口圆角半径（px）。默认 12：niri 这类只为客户端画焦点环/阴影的合成器不会再裁圆角，
// 所以由客户端自己画。想改就改这里（不做图形化开关）；设 0 回到直角窗口。
// 只在「没有系统装饰」的会话启用——有标题栏时只圆客户端区域反而更难看。
static constexpr int kWindowRadius = 12;

// ---------------------------------------------------------------- 主题 ----
// 一套 tokens 生成 QSS；所有颜色都从 QPalette 推，这样换配色只需要换 palette：
//   - 系统主题（Breeze / GTK / 自定义配色）→ Qt 给的默认 palette
//   - Noctalia（Material You）→ paletteFromM3() 从 ~/.cache/nyxuri/palette.toml 造一份
struct Theme {
    QString window, card, border, text, muted, accent, accentHover, accentSoft, input, log, hover;
};

static QString rgbaOf(const QColor& c, double alpha) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(int(alpha * 255));
}

static QColor mixColor(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t,
                            a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
}

static Theme themeFor(const QPalette& pal) {
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    const QColor win = pal.color(QPalette::Window);
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::WindowText);

    QColor muted = pal.color(QPalette::PlaceholderText);
    if (!muted.isValid()) muted = mixColor(text, win, 0.45);
    QColor accent = pal.color(QPalette::Accent);
    if (!accent.isValid()) accent = pal.color(QPalette::Highlight);

    const QColor border = mixColor(win, text, 0.16);
    const QColor log = mixColor(base, win, 0.5);

    return {
        win.name(),
        base.name(),
        border.name(),
        text.name(),
        muted.name(),
        accent.name(),
        (dark ? accent.lighter(115) : accent.darker(112)).name(),
        rgbaOf(accent, 0.16),
        base.name(),
        log.name(),
        rgbaOf(text, 0.07),
    };
}

// Material You 色板 → QPalette。之后 themeFor()、MemoryBar、各处内联颜色都按它走。
// 找不到某个角色时退回 Material 的近似角色，避免整块变黑。
static QPalette paletteFromM3(const core::M3Palette& m) {
    auto c = [&](const char* key, const char* fallbackKey) {
        return QColor(QString::fromStdString(m.get(key, m.get(fallbackKey))));
    };
    QPalette p;
    const QColor text = c("on_surface", "on_background");
    const QColor accent = c("primary", "on_surface");
    p.setColor(QPalette::Window, c("background", "surface"));
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, c("surface_container_lowest", "surface"));
    p.setColor(QPalette::AlternateBase, c("surface_container", "surface"));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, c("surface_container", "surface"));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, c("on_primary", "on_surface"));
    p.setColor(QPalette::PlaceholderText, c("on_surface_variant", "on_surface"));
    p.setColor(QPalette::Accent, accent);
    p.setColor(QPalette::BrightText, c("error", "on_surface"));
    p.setColor(QPalette::ToolTipBase, c("surface_container_high", "surface"));
    p.setColor(QPalette::ToolTipText, text);
    return p;
}

// QSS 里没法画三角形，就现生成一张 14x14 的尖角图给 QComboBox 用（跟随主题色）
static QString makeChevron(const QString& dir, const QColor& color) {
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.drawLine(QPointF(3.5, 5.5), QPointF(7.0, 9.0));
    p.drawLine(QPointF(7.0, 9.0), QPointF(10.5, 5.5));
    p.end();
    const QString path = dir + "/chevron.png";
    pm.save(path);
    return path;
}

static QString buildStyleSheet(const Theme& t, const QString& chevron) {
    QString qss = QStringLiteral(R"QSS(
QWidget { color: @text@; }
QMainWindow, QStackedWidget, QStatusBar { background: transparent; }
QDialog { background: @window@; }
QToolTip { background: @card@; color: @text@; border: 1px solid @border@; padding: 4px 6px; }

/* 顶部导航：一排标签，选中态是浅蓝药丸 */
QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent; color: @muted@; border: none;
    padding: 8px 16px; margin: 3px 4px 3px 0; border-radius: 9px;
}
QTabBar::tab:hover { background: @hover@; color: @text@; }
QTabBar::tab:selected { background: @accentSoft@; color: @accent@; font-weight: 600; }

/* 页面大标题与正文同款字体（不单独指定 family，避免中文字形回落不一致），
   只用字号和 600 字重做层级，700 的合成粗体会让 CJK 笔画糊成一团 */
QLabel#pageTitle { font-size: 19px; font-weight: 600; }
QLabel#pageSubtitle { color: @muted@; font-size: 12px; }
QLabel#muted { color: @muted@; }

/* 卡片 */
QGroupBox {
    background: @card@; border: 1px solid @border@; border-radius: 12px;
    margin-top: 0px; padding: 16px 18px 16px 18px;
}
QLabel#cardTitle {
    font-size: 13px; font-weight: 700; padding-bottom: 6px;
    border-bottom: 1px solid @border@; margin-bottom: 8px;
}
QScrollArea { background: transparent; border: none; }
QWidget#qt_scrollarea_viewport { background: transparent; }
QFrame#gateCard { background: @card@; border: 1px solid @border@; border-radius: 14px; }

/* 按钮 */
QPushButton {
    background: @card@; color: @text@; border: 1px solid @border@; border-radius: 8px;
    padding: 6px 14px; min-height: 18px;
}
QPushButton:hover { border-color: @accent@; color: @accent@; }
QPushButton:pressed { background: @accentSoft@; }
QPushButton:disabled { color: @muted@; background: @window@; }
QPushButton#primary {
    background: @accent@; color: #ffffff; border: 1px solid @accent@; font-weight: 600;
    padding: 7px 18px;
}
QPushButton#primary:hover { background: @accentHover@; border-color: @accentHover@; color: #ffffff; }
QPushButton#primary:disabled { background: @border@; border-color: @border@; color: @muted@; }

/* 输入 */
QLineEdit, QComboBox, QSpinBox {
    background: @input@; border: 1px solid @border@; border-radius: 8px;
    padding: 5px 8px; min-height: 18px; selection-background-color: @accent@; selection-color: #ffffff;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: @accent@; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { color: @muted@; background: @window@; }
QSpinBox::up-button, QSpinBox::down-button { width: 0; border: none; }
QComboBox::drop-down {
    subcontrol-origin: padding; subcontrol-position: center right;
    width: 24px; border: none; background: transparent;
}
QComboBox::down-arrow { image: url(@chevron@); width: 14px; height: 14px; }
QSpinBox { padding-right: 6px; }
QComboBox QAbstractItemView {
    background: @card@; border: 1px solid @border@; selection-background-color: @accentSoft@;
    selection-color: @accent@; outline: none;
}

/* 滑块 / 进度 */
QSlider::groove:horizontal { height: 6px; border-radius: 3px; background: @border@; }
QSlider::sub-page:horizontal { background: @accent@; border-radius: 3px; }
QSlider::handle:horizontal {
    width: 14px; height: 14px; margin: -5px 0; border-radius: 8px;
    background: @card@; border: 2px solid @accent@;
}
QSlider::sub-page:horizontal:disabled { background: @border@; }
QSlider::handle:horizontal:disabled { border-color: @border@; background: @window@; }
QCheckBox:disabled { color: @muted@; }
QProgressBar {
    background: @border@; border: none; border-radius: 6px; min-height: 12px;
    max-height: 12px; text-align: center; color: @muted@; font-size: 11px;
}
QProgressBar::chunk { background: @accent@; border-radius: 6px; }

/* 表格 / 日志 */
QTableWidget {
    background: @card@; border: 1px solid @border@; border-radius: 10px;
    gridline-color: transparent; alternate-background-color: @window@;
}
QHeaderView::section {
    background: @window@; color: @muted@; border: none;
    border-bottom: 1px solid @border@; padding: 6px 8px; font-weight: 600;
}
QTableCornerButton::section { background: @window@; border: none; }
QTableWidget::item { padding: 4px 6px; }
QTableWidget::item:selected { background: @accentSoft@; color: @text@; }
QPlainTextEdit, QTextEdit {
    background: @log@; border: 1px solid @border@; border-radius: 10px; padding: 8px;
    font-family: "JetBrains Mono", "Noto Sans Mono CJK SC", monospace; font-size: 12px;
}

/* 滚动条 */
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @border@; border-radius: 4px; min-height: 26px; }
QScrollBar::handle:vertical:hover { background: @muted@; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: @border@; border-radius: 4px; min-width: 26px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* 状态栏 */
QStatusBar { background: transparent; color: @muted@; border-top: 1px solid @border@; }
QStatusBar::item { border: none; }
)QSS");

    const std::vector<std::pair<QString, QString>> tokens = {
        {"@window@", t.window}, {"@card@", t.card}, {"@border@", t.border},
        {"@text@", t.text},     {"@muted@", t.muted}, {"@accentHover@", t.accentHover},
        {"@accentSoft@", t.accentSoft}, {"@accent@", t.accent},
        {"@input@", t.input},   {"@log@", t.log},     {"@hover@", t.hover},
        {"@chevron@", chevron},
    };
    for (const auto& kv : tokens) qss.replace(kv.first, kv.second);
    return qss;
}

// 统计目录里的文件数与字节数（清缓存前给个交代）
static void dirUsage(const std::filesystem::path& root, int& files, uint64_t& bytes) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            bytes += static_cast<uint64_t>(it->file_size(ec));
            files++;
        }
    }
}

// 卡片标题用普通 QLabel 放进布局，不依赖 QGroupBox::title（不同 Qt 风格定位会跑偏）
static void setCardTitle(QGroupBox* box, QLayout* lay, const QString& text) {
    box->setTitle("");
    auto* t = new QLabel(text, box);
    t->setObjectName("cardTitle");
    if (auto* vb = qobject_cast<QVBoxLayout*>(lay)) vb->insertWidget(0, t);
    else if (auto* fm = qobject_cast<QFormLayout*>(lay)) fm->insertRow(0, t);
}

// 切页过渡用的临时贴图：画上一页的快照，透明度从 1 收到 0。
// 自己画（而不是 QGraphicsOpacityEffect）是为了保住窗口自绘的圆角底色。
class FadeOverlay : public QWidget {
public:
    FadeOverlay(QWidget* parent, const QPixmap& pix) : QWidget(parent), pix_(pix) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setGeometry(parent->rect());
        show();    // 直接 new 出来的子控件默认是隐藏的，不 show 就永远看不到
        raise();
    }
    void setFade(qreal f) { fade_ = f; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setOpacity(fade_);
        p.drawPixmap(0, 0, pix_);
    }

private:
    QPixmap pix_;
    qreal fade_ = 1.0;
};

// 每个页面顶部的标题 + 一句说明
static void addPageHeader(QVBoxLayout* v, const QString& title, const QString& subtitle) {
    auto* t = new QLabel(title);
    t->setObjectName("pageTitle");
    auto* s = new QLabel(subtitle);
    s->setObjectName("pageSubtitle");
    s->setWordWrap(true);
    v->addWidget(t);
    v->addWidget(s);
    v->addSpacing(2);
}

// 四段式横条：程序占用（蓝）→ 缓存（浅蓝）→ 游戏 -Xmx（浅灰，长度就是分配量）→ 余下可用。
// 灰段从浅蓝右边缘开始，琥珀三角标在灰段右端，一眼看出「分了多少、还剩多少」。
class MemoryBar : public QWidget {
public:
    explicit MemoryBar(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(32);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setStats(const core::MemInfo& mem, int plannedMb) {
        mem_ = mem;
        planned_ = plannedMb;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const MemColors c = memColors(palette());
        const bool dark = palette().color(QPalette::Window).lightness() < 128;

        QRectF r = rect().adjusted(0, 8, 0, -4);
        const qreal radius = r.height() / 2;
        QPainterPath clip;
        clip.addRoundedRect(r, radius, radius);
        p.setClipPath(clip);
        p.fillRect(r, c.track);

        const double total = mem_.totalMb > 0 ? static_cast<double>(mem_.totalMb) : 1.0;
        auto seg = [&](double fromMb, double toMb, const QColor& color) {
            if (toMb <= fromMb) return;
            QRectF s(r.left() + r.width() * fromMb / total, r.top(),
                     r.width() * (toMb - fromMb) / total, r.height());
            p.fillRect(s, color);
        };
        const double used = std::min<double>(static_cast<double>(mem_.usedMb), total);
        const double cache = std::min<double>(static_cast<double>(mem_.cachedMb), total - used);
        const double gameMb = planned_ > 0 ? static_cast<double>(planned_) : 0.0;
        // 游戏段紧接缓存之后；分得多到超出总内存时，缓存先让位（它本来就是可回收的），
        // 这样灰段长度始终等于 -Xmx，不会被截断成半截。
        double cacheView = cache;
        double gameStart = used + cacheView;
        double gameEnd = gameStart + gameMb;
        if (gameEnd > total) {
            cacheView = std::max(0.0, cacheView - (gameEnd - total));
            gameStart = used + cacheView;
            gameEnd = std::min(total, gameStart + gameMb);
        }
        seg(0, used, c.used);
        seg(used, used + cacheView, c.cache);
        seg(gameStart, gameEnd, c.game);

        // -Xmx 右边界：条内竖线 + 顶部下指小三角
        if (planned_ > 0) {
            const double x = r.left() + r.width() * gameEnd / total;
            p.setClipping(false);
            p.setPen(QPen(dark ? QColor("#111214") : QColor("#ffffff"), 5));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.setPen(QPen(c.mark, 2));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            QPolygonF caret;
            const qreal cx = std::clamp(x, r.left() + 5, r.right() - 5);
            caret << QPointF(cx - 5, 0) << QPointF(cx + 5, 0) << QPointF(cx, 7);
            p.setPen(Qt::NoPen);
            p.setBrush(c.mark);
            p.drawPolygon(caret);
        }
    }

private:
    core::MemInfo mem_;
    int planned_ = 0;
};

// ---------------------------------------------------------------- 主窗口 ----
class LauncherWindow : public QMainWindow {
public:
    explicit LauncherWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        const std::string exe = QCoreApplication::applicationFilePath().toStdString();
        // 应用目录 = ELF 所在目录；装进 /usr/bin 这类只读位置时退回 XDG 数据目录，
        // 否则 .mc263/ 与 minecraft/ 根本建不出来。
        std::filesystem::path root = core::Layout::detect(exe).appDir;
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec || ::access(root.c_str(), W_OK) != 0) {
            const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            if (!data.isEmpty()) root = std::filesystem::path(data.toStdString());
        }
        dataRoot_ = root;   // 可写的数据根：应用目录，或只读安装时的 XDG 数据目录
        layout_ = core::Layout::at(root);
        std::string err;
        cfg_.load(layout_.configFile, err);  // 配置路径只跟应用目录有关，可以先读
        // 配置里存过游戏目录：能写且过守卫才用，否则回退到默认的 <应用目录>/minecraft
        if (!cfg_.gameDir.empty()) {
            core::Layout cand = core::Layout::at(dataRoot_, cfg_.gameDir);
            std::string gerr;
            if (cand.writable(gerr)) layout_ = cand;
            else cfg_.gameDir.clear();
        }
        if (!layout_.ensure(err)) {
            QMessageBox::critical(this, "路径被拒绝", QString::fromStdString(err));
        }
        defaultPalette_ = qApp->palette();
        requiredJava_ = core::requiredJavaMajor(layout_, cfg_.version);
        saveDownloaderScript();
        applyPalette();   // 先定调色板：buildUi 里的内联颜色会跟着它走
        buildUi();
        applyStyleSheet();
        QTimer::singleShot(700, this, [this] { checkGameDirs(); });
        loadingConfig_ = true;
        loadConfigToUi();
        loadingConfig_ = false;
        setupAutoSave();
        refreshLocalVersions();
        refreshJava();
        statusBar()->showMessage("游戏目录 " + QString::fromStdString(layout_.gameDir.string()) +
                                 " | 配置 " + QString::fromStdString(layout_.configFile.string()) +
                                 "（改动即自动保存）");
    }

    // 后台线程回填 UI（QPointer 保证窗口销毁后不会访问悬空 this）
    void applyJavaList(const std::vector<core::JavaInfo>& list) {
        javas_ = list;
        javaTable_->setRowCount(0);
        javaCombo_->clear();
        for (const auto& j : javas_) {
            int row = javaTable_->rowCount();
            javaTable_->insertRow(row);
            javaTable_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(j.path.string())));
            javaTable_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(j.version)));
            javaTable_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(j.source)));
            javaTable_->setItem(row, 3, new QTableWidgetItem(j.valid ? "可用" : "不可用"));
            if (j.valid) javaCombo_->addItem(QString::fromStdString(j.path.string()));
        }
        javaTable_->resizeColumnsToContents();
        if (!cfg_.javaPath.empty()) {
            int i = javaCombo_->findText(QString::fromStdString(cfg_.javaPath));
            if (i >= 0) javaCombo_->setCurrentIndex(i);
        }
        javaState_->setText("找到 " + QString::number(javas_.size()) + " 个 Java（后台线程扫描）");
        // 闸门判定：任意一个满足该版本要求的 Java 就放行；否则记住最高的旧版本用于提示
        // （26.x 要 25、1.20+ 要 21、1.17~1.19 要 17）
        javaScanDone_ = true;
        javaUsable_ = false;
        javaOldMajor_ = 0;
        javaOldVersion_.clear();
        for (const auto& j : javas_) {
            if (!j.valid) continue;
            if (j.major >= requiredJava_) javaUsable_ = true;
            else if (j.major > javaOldMajor_) {
                javaOldMajor_ = j.major;
                javaOldVersion_ = j.version;
            }
        }
        applyJavaGate();
    }
    void applyCompat(const std::vector<core::CheckItem>& items) {
        compatTable_->setRowCount(0);
        int okCount = 0;
        for (const auto& it : items) {
            int row = compatTable_->rowCount();
            compatTable_->insertRow(row);
            compatTable_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(it.name)));
            QTableWidgetItem* st = new QTableWidgetItem(it.ok ? "通过" : "未通过");
            st->setForeground(it.ok ? QColor("#15803d") : QColor("#b91c1c"));
            compatTable_->setItem(row, 1, st);
            compatTable_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(it.detail)));
            if (it.ok) ++okCount;
        }
        compatTable_->resizeColumnsToContents();
        compatState_->setText(QString::number(okCount) + "/" + QString::number(items.size()) + " 项通过");
    }
    void applyOnlineVersions(const std::vector<std::string>& versions, const QString& err) {
        if (!err.isEmpty()) { downloadLog_->appendPlainText("在线版本获取失败: " + err); return; }
        QString cur = downloadVersion_->currentText();
        downloadVersion_->clear();
        for (const auto& v : versions) downloadVersion_->addItem(QString::fromStdString(v));
        if (!cur.isEmpty()) downloadVersion_->setCurrentText(cur);
        downloadLog_->appendPlainText("在线版本列表已更新（" + QString::number(versions.size()) + " 个）");
    }

protected:
    void closeEvent(QCloseEvent* ev) override {
        collectUiToConfig();
        std::string err;
        cfg_.save(layout_.configFile, err);
        if (downloadProc_ && downloadProc_->state() != QProcess::NotRunning) downloadProc_->terminate();
        if (launchProc_ && launchProc_->state() != QProcess::NotRunning) launchProc_->terminate();
        QMainWindow::closeEvent(ev);
    }

private:
    void saveDownloaderScript() {
        std::string err;
        scriptPath_ = layout_.configDir / "mc263_downloader.py";
        mc263::fsx::writeText(scriptPath_, kDownloaderPy, err);
        ::chmod(scriptPath_.c_str(), 0755);
    }
    static QString pythonBin() {
        QString p = QString::fromLocal8Bit(qgetenv("MC263_PYTHON"));
        return p.isEmpty() ? QStringLiteral("python3") : p;
    }

    // 圆角窗口：paintEvent 自己画一层圆角背景，QSS 里 QMainWindow 是透明的。
    // 只在没有系统装饰时启用（niri 的 prefer-no-csd / QT_WAYLAND_DISABLE_WINDOWDECORATION=1）：
    // 有 CSD/SSD 时圆角只作用在客户端区域，反而难看，保持系统外观。
    static bool noServerDecorations() {
        if (qEnvironmentVariableIntValue("QT_WAYLAND_DISABLE_WINDOWDECORATION") == 1) return true;
        return core::isNiriSession();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(theme_.window.isEmpty() ? palette().color(QPalette::Window)
                                           : QColor(theme_.window));
        if (roundedCorners_) {
            p.drawRoundedRect(QRectF(rect()), kWindowRadius, kWindowRadius);
        } else {
            p.fillRect(rect(), p.brush());
        }
    }

    void buildUi() {
        roundedCorners_ = kWindowRadius > 0 && noServerDecorations();
        if (roundedCorners_) {
            setAttribute(Qt::WA_TranslucentBackground, true);
            setAttribute(Qt::WA_NoSystemBackground, true);
        }
        setWindowTitle("mc263 · Minecraft 26.3 启动器");
        resize(980, 680);
        auto* central = new QWidget(this);
        auto* v = new QVBoxLayout(central);
        v->setContentsMargins(10, 8, 10, 0);
        v->setSpacing(6);
        // 顶部横向导航：一排标签，页面内容占满整宽
        nav_ = new QTabBar(this);
        nav_->setObjectName("nav");
        nav_->setExpanding(false);
        nav_->setDrawBase(false);
        nav_->setUsesScrollButtons(false);
        nav_->setMovable(false);
        nav_->setFocusPolicy(Qt::NoFocus);
        nav_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        for (const char* s : {"启动游戏", "下载游戏", "Mod 下载", "Java 与兼容性", "游戏目录", "设置 / 内存"})
            nav_->addTab(QString::fromUtf8(s));
        stack_ = new QStackedWidget(this);
        // 每页放进滚动区：窗口缩小/字体变大时滚动，而不是把卡片内容挤到重叠
        auto addPage = [this](QWidget* page) {
            if (auto* lay = page->layout()) {
                lay->setContentsMargins(22, 16, 22, 16);
                if (auto* vb = qobject_cast<QVBoxLayout*>(lay)) vb->setSpacing(10);
            }
            auto* sa = new QScrollArea(this);
            sa->setWidgetResizable(true);
            sa->setFrameShape(QFrame::NoFrame);
            sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            sa->setWidget(page);
            stack_->addWidget(sa);
        };
        addPage(pageLaunch());
        addPage(pageDownload());
        addPage(pageMods());
        addPage(pageJavaCompat());
        addPage(pageDirs());
        addPage(pageSettings());
        v->addWidget(nav_);
        v->addWidget(stack_, 1);
        mainPage_ = central;

        // 启动闸门：Java 不达标时用整页遮住主界面（不是弹窗，避免点穿）
        gatePage_ = pageJavaGate();
        rootStack_ = new QStackedWidget(this);
        rootStack_->addWidget(mainPage_);
        rootStack_->addWidget(gatePage_);
        setCentralWidget(rootStack_);
        rootStack_->setCurrentWidget(gatePage_);
        connect(nav_, &QTabBar::currentChanged, this, &LauncherWindow::switchPage);
        nav_->setCurrentIndex(0);
    }

    // Java 闸门页：启动时先遮住主界面，扫描结果出来再决定放行还是留在这里
    QWidget* pageJavaGate() {
        auto* page = new QWidget(this);
        auto* outer = new QVBoxLayout(page);
        outer->setContentsMargins(40, 40, 40, 40);
        outer->addStretch(1);

        auto* card = new QFrame(page);
        card->setFrameShape(QFrame::StyledPanel);
        card->setMinimumWidth(620);
        card->setMaximumWidth(680);
        card->setAutoFillBackground(true);
        QPalette pal = card->palette();
        pal.setColor(QPalette::Window, palette().color(QPalette::Base));
        card->setPalette(pal);
        auto* shadow = new QGraphicsDropShadowEffect(card);
        shadow->setBlurRadius(28);
        shadow->setOffset(0, 4);
        shadow->setColor(QColor(0, 0, 0, 60));
        card->setGraphicsEffect(shadow);
        auto* cv = new QVBoxLayout(card);
        cv->setContentsMargins(30, 26, 30, 26);
        cv->setSpacing(14);

        gateTitle_ = new QLabel("正在检测 Java 运行时…", card);
        QFont tf = gateTitle_->font();
        tf.setPointSize(tf.pointSize() + 6);
        tf.setBold(true);
        gateTitle_->setFont(tf);
        cv->addWidget(gateTitle_);

        gateStatus_ = new QLabel(card);
        gateStatus_->setWordWrap(true);
        gateStatus_->setTextFormat(Qt::RichText);
        cv->addWidget(gateStatus_);

        gateDetail_ = new QLabel(card);
        gateDetail_->setWordWrap(true);
        gateDetail_->setStyleSheet("color:" +
                                   (palette().color(QPalette::Window).lightness() < 128
                                        ? QStringLiteral("#9aa4b2") : QStringLiteral("#5b6472")) + ";");
        cv->addWidget(gateDetail_);

        auto* row = new QHBoxLayout();
        gateDownloadBtn_ = new QPushButton("下载 Java 运行时", card);
        gateDownloadBtn_->setObjectName("primary");
        gateDownloadBtn_->setDefault(true);
        auto* rescan = new QPushButton("重新检测", card);
        auto* manual = new QPushButton("手动指定 java…", card);
        auto* quit = new QPushButton("退出", card);
        row->addWidget(gateDownloadBtn_);
        row->addWidget(rescan);
        row->addWidget(manual);
        row->addStretch(1);
        row->addWidget(quit);
        cv->addLayout(row);

        outer->addWidget(card, 0, Qt::AlignHCenter);
        outer->addStretch(1);

        connect(gateDownloadBtn_, &QPushButton::clicked, this, &LauncherWindow::goDownloadJava);
        connect(rescan, &QPushButton::clicked, this, [this] {
            gateStatus_->setText("正在重新检测…");
            refreshJava();
        });
        connect(manual, &QPushButton::clicked, this, [this] {
            QString p = QFileDialog::getOpenFileName(this, "选择 java 可执行文件", "/usr/bin");
            if (p.isEmpty()) return;
            cfg_.javaPath = p.toStdString();
            std::string err;
            cfg_.save(layout_.configFile, err);
            gateStatus_->setText("正在检测 " + p + " …");
            refreshJava();
        });
        connect(quit, &QPushButton::clicked, this, &QWidget::close);
        return page;
    }

    // 放行 or 挡住：由最近一次 Java 扫描结果决定（阈值随版本走：26.x=25、1.20+=21、1.17+=17）
    void applyJavaGate() {
        if (!rootStack_ || !gatePage_ || !mainPage_) return;
        if (javaScanDone_ && javaUsable_) {
            rootStack_->setCurrentWidget(mainPage_);
            return;
        }
        const QString ver = QString::fromStdString(cfg_.version);
        const QString need = QString::number(requiredJava_);
        if (!javaScanDone_) {
            gateTitle_->setText("正在检测 Java 运行时…");
            gateStatus_->setText(QString("Minecraft %1 需要 <b>Java %2 及以上</b>。").arg(ver, need));
            gateDetail_->setText("正在扫描配置、Mojang 运行时、JAVA_HOME、PATH 与 /usr/lib/jvm。");
            rootStack_->setCurrentWidget(gatePage_);
            return;
        }
        if (javaOldMajor_ > 0) {
            gateTitle_->setText(QString("需要 Java %1 及以上（当前 Java %2）").arg(need).arg(javaOldMajor_));
            if (javaOldMajor_ <= 11) {
                gateStatus_->setText(
                    QString("当前 <b>Java %1</b>（%2）已不再被 Minecraft %3 所支持。")
                        .arg(javaOldMajor_)
                        .arg(QString::fromStdString(javaOldVersion_), ver));
            } else {
                gateStatus_->setText(
                    QString("当前 <b>Java %1</b>（%2）低于 Minecraft %3 要求的 Java %4。")
                        .arg(javaOldMajor_)
                        .arg(QString::fromStdString(javaOldVersion_), ver, need));
            }
            gateDetail_->setText(QString("Minecraft %1 需要 Java %2 及以上（26.x 要 25、1.20+ 要 21、1.17+ 要 17）。"
                                         "可以下载 Mojang 官方 Java 25 运行时（约 200 MB），"
                                         "也可以手动指定本机更高版本的 java。")
                                     .arg(ver, need));
        } else {
            gateTitle_->setText("没有检测到可用的 Java");
            gateStatus_->setText(QString("Minecraft %1 需要 <b>Java %2 及以上</b>，当前系统里没有可用的 java。")
                                     .arg(ver, need));
            gateDetail_->setText("点「下载 Java 运行时」去下载页：不满足要求时，"
                                 "「Java 运行时」这一项会显示为蓝色并默认勾选，下载游戏时一并获取 Mojang Java 25。");
        }
        applyJavaAvailabilityToDownload();
        rootStack_->setCurrentWidget(gatePage_);
    }

    // 下载页的「Java 运行时」只在没有可用 Java 时出现，蓝色高亮 + 默认勾选
    void applyJavaAvailabilityToDownload() {
        if (!runtimeCheck_) return;
        const bool need = javaScanDone_ && !javaUsable_;
        runtimeCheck_->setVisible(need);
        runtimeCheck_->setChecked(need);
    }

    void goDownloadJava() {
        if (rootStack_ && mainPage_) rootStack_->setCurrentWidget(mainPage_);
        if (runtimeCheck_) {
            runtimeCheck_->setVisible(true);
            runtimeCheck_->setChecked(true);
        }
        if (nav_) nav_->setCurrentIndex(1);
    }

    QWidget* pageLaunch() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "启动游戏", "选本地版本、填玩家名，日志实时输出；缺 Java 会先被闸门拦住");
        auto* box = new QGroupBox("原版启动", w);
        auto* form = new QFormLayout(box);
        setCardTitle(box, form, "原版启动");
        launchVersion_ = new QComboBox(w); launchVersion_->setEditable(true);
        usernameEdit_ = new QLineEdit(w);
        form->addRow("游戏版本", launchVersion_);
        form->addRow("玩家名", usernameEdit_);
        v->addWidget(box);
        auto* row = new QHBoxLayout();
        launchBtn_ = new QPushButton("启动游戏", w);
        launchBtn_->setObjectName("primary");
        auto* refreshBtn = new QPushButton("刷新本地版本", w);
        auto* openDirBtn = new QPushButton("打开游戏目录", w);
        row->addWidget(launchBtn_); row->addWidget(refreshBtn); row->addWidget(openDirBtn); row->addStretch(1);
        v->addLayout(row);
        launchState_ = new QLabel("就绪", w);
        v->addWidget(launchState_);
        launchLog_ = new QPlainTextEdit(w); launchLog_->setReadOnly(true);
        v->addWidget(launchLog_, 1);
        connect(launchBtn_, &QPushButton::clicked, this, &LauncherWindow::startLaunch);
        connect(refreshBtn, &QPushButton::clicked, this, &LauncherWindow::refreshLocalVersions);
        connect(openDirBtn, &QPushButton::clicked, this, [this] {
            QProcess::startDetached("xdg-open", {QString::fromStdString(layout_.gameDir.string())});
        });
        return w;
    }

    QWidget* pageDownload() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "下载游戏", "从 Mojang 官方 CDN 或镜像拉取 26.3：SHA1 校验 + 多线程 + 断点缓存");
        auto* box = new QGroupBox("下载来源", w);
        auto* form = new QFormLayout(box);
        setCardTitle(box, form, "下载来源");
        downloadVersion_ = new QComboBox(w); downloadVersion_->setEditable(true);
        downloadVersion_->addItem("26.3");
        mirrorCombo_ = new QComboBox(w);
        mirrorCombo_->addItem("official · Mojang 官方", "official");
        mirrorCombo_->addItem("bmclapi · 国内镜像", "bmclapi");
        jobsSpin_ = new QSpinBox(w); jobsSpin_->setRange(1, 16); jobsSpin_->setValue(10);
        auto* onlineBtn = new QPushButton("获取在线版本", w);
        auto* verRow = new QHBoxLayout(); verRow->addWidget(downloadVersion_, 1); verRow->addWidget(onlineBtn);
        form->addRow("版本", verRow);
        form->addRow("下载源", mirrorCombo_);
        form->addRow("并发线程", jobsSpin_);
        auto* compBox = new QWidget(w);
        auto* compLayout = new QHBoxLayout(compBox);
        compLayout->setContentsMargins(0, 0, 0, 0);
        struct CompOpt {
            const char* key;
            const char* label;
            const char* tip;
        };
        for (const CompOpt& o : {
                 CompOpt{"jar", "客户端 jar",
                         "游戏本体：versions/<版本>/<版本>.jar（含 net.minecraft.client.main.Main）。\n"
                         "不勾就没有可启动的程序，启动页会报找不到主类。"},
                 CompOpt{"libraries", "依赖库 + natives",
                         "libraries/ 下的 Java 依赖（LWJGL、log4j、authlib 等 100+ 个 jar）\n"
                         "以及 natives/ 里的 Linux .so（图形、音频、输入）。\n"
                         "缺了会 NoClassDefFoundError 或 GLFW/OpenAL 初始化失败。"},
                 CompOpt{"assets", "游戏资源",
                         "assets/objects 与 indexes：声音、语言文件、贴图、字体，5000+ 个小文件。\n"
                         "缺了仍能进游戏，但没有音效、部分界面缺字。"}}) {
            auto* cb = new QCheckBox(QString::fromUtf8(o.label), compBox);
            cb->setProperty("comp", QString::fromLatin1(o.key));
            cb->setToolTip(QString::fromUtf8(o.tip));
            compChecks_.push_back(cb);
            compLayout->addWidget(cb);
        }
        // Java 运行时不再是一个常驻选项：只有启动时没有满足版本要求的 Java 才出现，
        // 蓝色高亮并默认勾选（见 applyJavaAvailabilityToDownload）。
        runtimeCheck_ = new QCheckBox("Java 运行时", compBox);
        runtimeCheck_->setProperty("comp", QStringLiteral("runtime"));
        runtimeCheck_->setToolTip(
            "Mojang 官方 Java 25（runtime/java-runtime-epsilon/），约 200 MB。\n"
            "系统里的 Java 达不到该版本要求时自动勾选；已满足要求时这一项不显示。");
        runtimeCheck_->setStyleSheet(QString("QCheckBox { color: %1; font-weight: 600; }")
                                         .arg(palette().color(QPalette::Window).lightness() < 128
                                                  ? QStringLiteral("#7cc0ff") : QStringLiteral("#1a5fd0")));
        runtimeCheck_->setChecked(true);
        runtimeCheck_->setVisible(false);
        compLayout->addWidget(runtimeCheck_);
        compLayout->addStretch(1);
        form->addRow("组件", compBox);
        auto* compHint = new QLabel("首次下载全勾；Java 达不到版本要求时会自动带上蓝色那项运行时，"
                                    "已存在且 SHA1 一致的文件自动跳过", w);
        compHint->setWordWrap(true);
        compHint->setStyleSheet("color:" +
                                (palette().color(QPalette::Window).lightness() < 128
                                     ? QStringLiteral("#9aa4b2") : QStringLiteral("#5b6472")) + ";");
        form->addRow(compHint);
        v->addWidget(box);
        auto* row = new QHBoxLayout();
        downloadBtn_ = new QPushButton("开始下载", w);
        downloadBtn_->setObjectName("primary");
        cancelBtn_ = new QPushButton("取消", w); cancelBtn_->setEnabled(false);
        clearCacheBtn_ = new QPushButton("清除下载缓存", w);
        clearCacheBtn_->setToolTip("只删元数据缓存（版本清单 / Modrinth 查询 / 加载器 profile），已下载的游戏文件不动");
        row->addWidget(downloadBtn_);
        row->addWidget(cancelBtn_);
        row->addWidget(clearCacheBtn_);
        row->addStretch(1);
        v->addLayout(row);
        downloadBar_ = new QProgressBar(w);
        downloadLabel_ = new QLabel("等待下载", w);
        v->addWidget(downloadBar_); v->addWidget(downloadLabel_);
        downloadLog_ = new QPlainTextEdit(w); downloadLog_->setReadOnly(true);
        v->addWidget(downloadLog_, 1);
        connect(downloadBtn_, &QPushButton::clicked, this, &LauncherWindow::startDownload);
        connect(cancelBtn_, &QPushButton::clicked, this, [this] {
            if (downloadProc_) downloadProc_->terminate();
        });
        connect(clearCacheBtn_, &QPushButton::clicked, this, &LauncherWindow::clearDownloadCache);
        connect(onlineBtn, &QPushButton::clicked, this, &LauncherWindow::refreshOnlineVersions);
        return w;
    }

    // ---- Mod 下载（Modrinth 公开 API）----
    QWidget* pageMods() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "Mod 下载", "从 Modrinth 搜索，按加载器与游戏版本过滤，直接装进 minecraft/mods/");

        auto* sbox = new QGroupBox("搜索", w);
        auto* form = new QFormLayout(sbox);
        setCardTitle(sbox, form, "搜索");
        modQuery_ = new QLineEdit(sbox);
        modQuery_->setPlaceholderText("sodium / lithium / jei …");
        modQuery_->setClearButtonEnabled(true);
        modLoader_ = new QComboBox(sbox);
        modLoader_->addItem("Fabric", "fabric");
        modLoader_->addItem("Forge", "forge");
        modLoader_->addItem("NeoForge", "neoforge");
        modLoader_->addItem("Quilt", "quilt");
        modLoader_->addItem("不限", "");
        modVersionEdit_ = new QLineEdit(sbox);
        modVersionEdit_->setText(QString::fromStdString(cfg_.version));
        modSort_ = new QComboBox(sbox);
        modSort_->addItem("下载量", "downloads");
        modSort_->addItem("相关度", "relevance");
        modSort_->addItem("关注数", "follows");
        modSort_->addItem("最新发布", "newest");
        modSort_->addItem("最近更新", "updated");
        form->addRow("关键词", modQuery_);
        form->addRow("加载器", modLoader_);
        form->addRow("游戏版本", modVersionEdit_);
        form->addRow("排序", modSort_);

        modSearchBtn_ = new QPushButton("搜索", sbox);
        modSearchBtn_->setObjectName("primary");
        auto* srow = new QHBoxLayout();
        srow->addWidget(modSearchBtn_);
        auto* hint = new QLabel("按下载量排序，取前 20 个；双击结果可直接安装（会自动补齐 required 前置）", sbox);
        hint->setObjectName("muted");
        srow->addWidget(hint);
        srow->addStretch(1);
        form->addRow(QString(), srow);
        v->addWidget(sbox);

        auto* lbox = new QGroupBox("加载器", w);
        auto* lform = new QFormLayout(lbox);
        setCardTitle(lbox, lform, "加载器（Fabric / Quilt）");
        loaderCombo_ = new QComboBox(lbox);
        loaderCombo_->addItem("Fabric", "fabric");
        loaderCombo_->addItem("Quilt", "quilt");
        loaderVerCombo_ = new QComboBox(lbox);
        auto* getLv = new QPushButton("获取版本", lbox);
        auto* lrow = new QHBoxLayout();
        lrow->addWidget(loaderVerCombo_, 1);
        lrow->addWidget(getLv);
        installLoaderBtn_ = new QPushButton("安装 / 更新加载器", lbox);
        installLoaderBtn_->setObjectName("primary");
        loaderState_ = new QLabel("装完后启动会用 versions/<版本>+<加载器>，原版不受影响", lbox);
        loaderState_->setObjectName("muted");
        lform->addRow("加载器", loaderCombo_);
        lform->addRow("版本", lrow);
        auto* irow = new QHBoxLayout();
        irow->addWidget(installLoaderBtn_);
        irow->addStretch(1);
        lform->addRow(QString(), irow);
        lform->addRow(QString(), loaderState_);
        v->addWidget(lbox);


        modTable_ = new QTableWidget(0, 4, w);
        modTable_->setHorizontalHeaderLabels({"Mod", "作者", "下载量", "说明"});
        modTable_->horizontalHeader()->setStretchLastSection(true);
        modTable_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        modTable_->horizontalHeader()->resizeSection(0, 230);
        modTable_->horizontalHeader()->resizeSection(1, 130);
        modTable_->horizontalHeader()->resizeSection(2, 90);
        modTable_->verticalHeader()->setVisible(false);
        modTable_->setShowGrid(false);
        modTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        modTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        modTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        modTable_->setAlternatingRowColors(true);
        v->addWidget(modTable_, 2);

        modInstallBtn_ = new QPushButton("安装选中 Mod", w);
        modInstallBtn_->setObjectName("primary");
        auto* openMods = new QPushButton("打开 mods 目录", w);
        auto* brow = new QHBoxLayout();
        brow->addWidget(modInstallBtn_);
        brow->addWidget(openMods);
        brow->addStretch(1);
        v->addLayout(brow);

        modState_ = new QLabel("输入关键词后点搜索", w);
        modState_->setObjectName("muted");
        v->addWidget(modState_);
        modLog_ = new QPlainTextEdit(w);
        modLog_->setReadOnly(true);
        modLog_->setMaximumBlockCount(500);
        v->addWidget(modLog_, 1);

        connect(modSearchBtn_, &QPushButton::clicked, this, &LauncherWindow::startModSearch);
        connect(modQuery_, &QLineEdit::returnPressed, this, &LauncherWindow::startModSearch);
        connect(modInstallBtn_, &QPushButton::clicked, this, &LauncherWindow::installSelectedMod);
        connect(modTable_, &QTableWidget::cellDoubleClicked, this,
                [this](int, int) { installSelectedMod(); });
        connect(getLv, &QPushButton::clicked, this, &LauncherWindow::refreshLoaderVersions);
        connect(installLoaderBtn_, &QPushButton::clicked, this, &LauncherWindow::installSelectedLoader);
        connect(loaderCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { refreshLoaderVersions(); });
        connect(openMods, &QPushButton::clicked, this, [this] {
            const auto dir = core::isolatedGameDir(layout_, cfg_) / "mods";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            QProcess::startDetached("xdg-open", {QString::fromStdString(dir.string())});
        });
        return w;
    }

    void startModSearch() {
        const QString q = modQuery_->text().trimmed();
        if (q.isEmpty()) { modState_->setText("先输入关键词"); return; }
        modSearchBtn_->setEnabled(false);
        modState_->setText("搜索中…");
        const std::string loader = modLoader_->currentData().toString().toStdString();
        const std::string ver = modVersionEdit_->text().trimmed().toStdString();
        const std::string sort = modSort_->currentData().toString().toStdString();
        const auto cache = layout_.configDir / "cache";
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, q, loader, ver, sort, cache] {
            std::vector<mc263::modrinth::Hit> hits;
            std::string err;
            const bool ok = mc263::modrinth::search(q.toStdString(), loader, ver, sort, cache, hits, err);
            QMetaObject::invokeMethod(qApp, [guard, ok, err, hits] {
                if (guard) guard->applyModHits(ok, err, hits);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void applyModHits(bool ok, const std::string& err, const std::vector<mc263::modrinth::Hit>& hits) {
        modSearchBtn_->setEnabled(true);
        modTable_->setRowCount(0);
        if (!ok) {
            modState_->setText("搜索失败：" + QString::fromStdString(err));
            return;
        }
        for (const auto& h : hits) {
            const int r = modTable_->rowCount();
            modTable_->insertRow(r);
            auto* name = new QTableWidgetItem(QString::fromStdString(h.title));
            name->setData(Qt::UserRole, QString::fromStdString(h.id));
            name->setToolTip(QString::fromStdString(h.id));
            modTable_->setItem(r, 0, name);
            modTable_->setItem(r, 1, new QTableWidgetItem(QString::fromStdString(h.author)));
            auto* dl = new QTableWidgetItem(QLocale().toString(static_cast<qulonglong>(h.downloads)));
            dl->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            modTable_->setItem(r, 2, dl);
            modTable_->setItem(r, 3, new QTableWidgetItem(QString::fromStdString(h.description)));
        }
        modState_->setText(hits.empty() ? "没有结果，换个关键词或放宽版本 / 加载器"
                                        : QString("找到 %1 个，双击或点「安装选中 Mod」").arg(hits.size()));
    }

    // ---- 安装 Mod：先解析 required 前置，确认后一起下载 ----
    // 解析（网络）在后台线程；弹框在主线程；下载再开一个后台线程，避免界面卡住。
    void installSelectedMod() {
        const int row = modTable_->currentRow();
        if (row < 0) { modState_->setText("先在列表里选一个 Mod"); return; }
        auto* nameItem = modTable_->item(row, 0);
        const std::string project = nameItem->data(Qt::UserRole).toString().toStdString();
        const QString title = nameItem->text();
        const std::string loader = modLoader_->currentData().toString().toStdString();
        const std::string ver = modVersionEdit_->text().trimmed().toStdString();
        modInstallBtn_->setEnabled(false);
        modState_->setText("解析 " + title + " 的前置…");
        const auto cache = layout_.configDir / "cache";
        const auto mods = core::isolatedGameDir(layout_, cfg_) / "mods";  // 版本隔离：mods 跟着版本走
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, project, loader, ver, cache, mods, title] {
            std::vector<mc263::modrinth::Version> list;
            std::string err;
            QString msg;
            bool ok = false;
            if (!mc263::modrinth::versions(project, loader, ver, cache, list, err)) {
                msg = "查版本失败：" + QString::fromStdString(err);
            } else if (list.empty()) {
                msg = "没有适配该版本 / 加载器的文件";
            } else {
                ok = true;
            }
            mc263::modrinth::DepPlan plan;
            if (ok && mc263::modrinth::primaryFile(list.front())) {
                mc263::modrinth::resolveDependencies(list.front(), loader, ver, cache, plan, err);
            } else if (ok) {
                ok = false;
                msg = "NOFILE";
            }
            QMetaObject::invokeMethod(qApp, [guard, ok, msg, plan, list, title, mods, project] {
                if (!guard) return;
                if (!ok) {
                    guard->modInstallBtn_->setEnabled(true);
                    guard->modState_->setText("失败");
                    if (msg == "NOFILE") {
                        guard->promptManualMod(title,
                            QString::fromStdString("https://modrinth.com/mod/" + project));
                    } else {
                        guard->modLog_->appendPlainText(QString("[%1] %2  %3")
                            .arg(QTime::currentTime().toString("HH:mm:ss"), title, msg));
                    }
                    return;
                }
                guard->askInstallPlan(title, list.front(), plan, mods);
            }, Qt::QueuedConnection);
        }).detach();
    }

    // 连本体都拿不到文件：按 Prism 的做法提示手动下载 + 打开模组网页
    void promptManualMod(const QString& title, const QString& url) {
        QMessageBox box(this);
        box.setWindowTitle("需要手动下载");
        box.setIcon(QMessageBox::Warning);
        box.setText(QString("%1 没有可用的下载文件（作者可能禁止了 API 分发）。\n\n"
                            "请到模组网页手动下载后放进 mods 目录：\n%2").arg(title, url));
        auto* open = box.addButton("打开网页", QMessageBox::ActionRole);
        box.addButton("关闭", QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == open)
            QDesktopServices::openUrl(QUrl(url));
        modState_->setText("需要手动下载");
    }

    void askInstallPlan(const QString& title, const mc263::modrinth::Version& root,
                        const mc263::modrinth::DepPlan& plan, const std::filesystem::path& mods) {
        QString text = QString("安装 %1（%2）到 %3")
                           .arg(title, QString::fromStdString(root.number),
                                QString::fromStdString(mods.string()));
        if (!plan.resolved.empty()) {
            text += QString("\n\n会自动补齐 %1 个前置：").arg(plan.resolved.size());
            for (const auto& d : plan.resolved)
                text += QString("\n  • %1 %2").arg(QString::fromStdString(d.title),
                                                    QString::fromStdString(d.versionNumber));
        }
        if (!plan.manual.empty()) {
            text += "\n\n以下前置无法自动下载，需要手动处理：";
            for (const auto& d : plan.manual)
                text += QString("\n  • %1（%2）\n    %3").arg(QString::fromStdString(d.title),
                                                               QString::fromStdString(d.reason),
                                                               QString::fromStdString(d.url));
        }
        QMessageBox box(this);
        box.setWindowTitle(plan.manual.empty() ? "安装 Mod" : "确认安装 / 需要手动下载前置");
        box.setIcon(plan.manual.empty() ? QMessageBox::Question : QMessageBox::Warning);
        box.setText(text);
        box.setTextFormat(Qt::PlainText);
        auto* go = box.addButton("安装", QMessageBox::AcceptRole);
        QAbstractButton* open = nullptr;
        if (!plan.manual.empty()) open = box.addButton("打开网页", QMessageBox::ActionRole);
        box.addButton("取消", QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == open && open) {
            QDesktopServices::openUrl(QUrl(QString::fromStdString(plan.manual.front().url)));
        } else if (box.clickedButton() != go) {
            modInstallBtn_->setEnabled(true);
            modState_->setText("已取消");
            return;
        }
        startModDownload(title, root, plan, mods);
    }

    void startModDownload(const QString& title, const mc263::modrinth::Version& root,
                          const mc263::modrinth::DepPlan& plan, const std::filesystem::path& mods) {
        struct Item { mc263::modrinth::File file; };
        std::vector<Item> items;
        if (const auto* f = mc263::modrinth::primaryFile(root)) items.push_back({*f});
        for (const auto& d : plan.resolved) items.push_back({d.file});

        QStringList manualNotes;
        for (const auto& d : plan.manual)
            manualNotes << QString("需要手动下载：%1  %2").arg(QString::fromStdString(d.title),
                                                           QString::fromStdString(d.url));

        modState_->setText(QString("下载 %1 个文件…").arg(items.size()));
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, items, mods, title, manualNotes] {
            std::error_code ec;
            std::filesystem::create_directories(mods, ec);
            QStringList lines;
            int failed = 0;
            for (const auto& it : items) {
                std::string err;
                bool cached = false;
                mc263::dl::Task t{it.file.url, mods / it.file.filename, it.file.sha1, 0644,
                                  it.file.filename};
                if (mc263::dl::fetchOne(t, "", 180, true, err, cached)) {
                    lines << QString("  %1%2").arg(QString::fromStdString(it.file.filename),
                                                   cached ? "（已存在，跳过）" : "");
                } else {
                    ++failed;
                    lines << QString("  %1 失败：%2").arg(QString::fromStdString(it.file.filename),
                                                          QString::fromStdString(err));
                }
            }
            lines += manualNotes;
            QMetaObject::invokeMethod(qApp, [guard, failed, lines, title, manualNotes] {
                if (!guard) return;
                guard->modInstallBtn_->setEnabled(true);
                guard->modState_->setText(failed == 0 ? "完成" : "部分失败");
                guard->modLog_->appendPlainText(QString("[%1] %2\n%3")
                    .arg(QTime::currentTime().toString("HH:mm:ss"), title, lines.join("\n")));
            }, Qt::QueuedConnection);
        }).detach();
    }

    QWidget* pageJavaCompat() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "Java 与兼容性", "列出所有 Java 及其来源，逐项检查架构、依赖库、natives 与图形音频库");
        auto* jbox = new QGroupBox("可用的 Java", w);
        auto* jv = new QVBoxLayout(jbox);
        setCardTitle(jbox, jv, "可用的 Java");
        javaTable_ = new QTableWidget(0, 4, w);
        javaTable_->setHorizontalHeaderLabels({"路径", "版本", "来源", "状态"});
        javaTable_->horizontalHeader()->setStretchLastSection(true);
        javaTable_->verticalHeader()->setVisible(false);
        javaTable_->setShowGrid(false);
        javaTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        jv->addWidget(javaTable_);
        javaCombo_ = new QComboBox(w);
        javaState_ = new QLabel("尚未扫描", w);
        auto* jrow = new QHBoxLayout();
        auto* rescan = new QPushButton("重新扫描", w);
        auto* manual = new QPushButton("手动指定 java", w);
        auto* useBtn = new QPushButton("设为当前 Java", w);
        jrow->addWidget(rescan); jrow->addWidget(manual); jrow->addWidget(useBtn); jrow->addStretch(1);
        jv->addLayout(jrow); jv->addWidget(new QLabel("当前选择", w)); jv->addWidget(javaCombo_);
        jv->addWidget(javaState_);
        v->addWidget(jbox, 2);
        auto* cbox = new QGroupBox("兼容性查验", w);
        auto* cv = new QVBoxLayout(cbox);
        setCardTitle(cbox, cv, "兼容性查验");
        compatTable_ = new QTableWidget(0, 3, w);
        compatTable_->setHorizontalHeaderLabels({"检查项", "状态", "说明"});
        compatTable_->horizontalHeader()->setStretchLastSection(true);
        compatTable_->verticalHeader()->setVisible(false);
        compatTable_->setShowGrid(false);
        compatTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        cv->addWidget(compatTable_);
        compatState_ = new QLabel("尚未检查", w);
        netCheck_ = new QCheckBox("同时检查官方下载源连通性", w);
        auto* checkBtn = new QPushButton("运行兼容性查验", w);
        auto* crow = new QHBoxLayout(); crow->addWidget(checkBtn); crow->addWidget(netCheck_); crow->addStretch(1);
        cv->addLayout(crow); cv->addWidget(compatState_);
        v->addWidget(cbox, 3);
        connect(rescan, &QPushButton::clicked, this, &LauncherWindow::refreshJava);
        connect(manual, &QPushButton::clicked, this, [this] {
            QString p = QFileDialog::getOpenFileName(this, "选择 java 可执行文件", "/usr/bin");
            if (!p.isEmpty()) { cfg_.javaPath = p.toStdString(); refreshJava(); }
        });
        connect(useBtn, &QPushButton::clicked, this, [this] {
            if (!javaCombo_->currentText().isEmpty()) {
                cfg_.javaPath = javaCombo_->currentText().toStdString();
                javaState_->setText("已选择 " + javaCombo_->currentText());
            }
        });
        connect(checkBtn, &QPushButton::clicked, this, &LauncherWindow::runCompat);
        return w;
    }

    QWidget* pageDirs() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "游戏目录", "自动扫描常见位置（ELF 旁 / rpm 的 XDG 目录 / ~/.minecraft，只列真实存在的），避免两套装各用一份、重复下载");

        auto* box = new QGroupBox("目录", w);
        auto* bv = new QVBoxLayout(box);
        setCardTitle(box, bv, "选择游戏目录");
        gameDirEdit_ = new QLineEdit(box);
        browseBtn_ = new QPushButton("浏览…", box);
        auto* drow = new QHBoxLayout();
        drow->addWidget(gameDirEdit_, 1);
        drow->addWidget(browseBtn_);
        bv->addLayout(drow);

        dirsTable_ = new QTableWidget(0, 4, box);
        dirsTable_->setHorizontalHeaderLabels({"目录", "版本数", "mods", "当前"});
        dirsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        dirsTable_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        dirsTable_->verticalHeader()->setVisible(false);
        dirsTable_->setShowGrid(false);
        dirsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        dirsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        dirsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        bv->addWidget(dirsTable_, 1);

        auto* row = new QHBoxLayout();
        auto* useBtn = new QPushButton("使用所选目录", box);
        useBtn->setObjectName("primary");
        auto* openBtn = new QPushButton("打开目录", box);
        auto* rescan = new QPushButton("重新扫描", box);
        row->addWidget(useBtn);
        row->addWidget(openBtn);
        row->addWidget(rescan);
        row->addStretch(1);
        bv->addLayout(row);

        dirsState_ = new QLabel(box);
        dirsState_->setObjectName("muted");
        bv->addWidget(dirsState_);
        v->addWidget(box, 1);

        connect(rescan, &QPushButton::clicked, this, &LauncherWindow::refreshGameDirs);
        connect(useBtn, &QPushButton::clicked, this, &LauncherWindow::applySelectedGameDir);
        connect(browseBtn_, &QPushButton::clicked, this, [this] {
            const QString d = QFileDialog::getExistingDirectory(this, "选择游戏目录", gameDirEdit_->text());
            if (d.isEmpty()) return;
            gameDirEdit_->setText(d);
            collectUiToConfig();
            saveConfigQuietly();
            refreshGameDirs();
            refreshLocalVersions();
        });
        connect(openBtn, &QPushButton::clicked, this, [this] {
            QProcess::startDetached("xdg-open", {QString::fromStdString(layout_.gameDir.string())});
        });
        refreshGameDirs();
        return w;
    }

    QWidget* pageSettings() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "设置 / 内存", "改动即时保存；内存默认交给 JVM 管理，需要精细控制时再关掉自动");
        core::MemInfo mi = core::readMemInfo();
        memTotalMb_ = mi.totalMb >= 2048 ? static_cast<int>(mi.totalMb) : 4096;
        if (mi.totalMb == 0) mi.totalMb = static_cast<uint64_t>(memTotalMb_);
        memInfo_ = mi;
        maxMemoryMb_ = std::max(2048, memTotalMb_ * 3 / 4);
        auto* mbox = new QGroupBox("内存图形化配置", w);
        auto* mv = new QVBoxLayout(mbox);
        setCardTitle(mbox, mv, "内存图形化配置");

        // 顶部：系统内存实况（其他程序占用 / 缓存 / 仅可用），每 2 秒刷新
        auto* head = new QHBoxLayout();
        memSummary_ = new QLabel(w);
        memSummary_->setTextFormat(Qt::RichText);
        auto* refreshMem = new QPushButton("刷新", w);
        refreshMem->setToolTip("重新读取 /proc/meminfo");
        head->addWidget(memSummary_, 1);
        head->addWidget(refreshMem);
        mv->addLayout(head);

        memBar_ = new MemoryBar(w);
        mv->addWidget(memBar_);

        auto* legend = new QHBoxLayout();
        usedStat_ = new QLabel(w);
        cacheStat_ = new QLabel(w);
        gameStat_ = new QLabel(w);
        availStat_ = new QLabel(w);
        for (QLabel* l : {usedStat_, cacheStat_, gameStat_, availStat_})
            l->setTextFormat(Qt::RichText);
        legend->addWidget(usedStat_);
        legend->addSpacing(16);
        legend->addWidget(cacheStat_);
        legend->addSpacing(16);
        legend->addWidget(gameStat_);
        legend->addSpacing(16);
        legend->addWidget(availStat_);
        legend->addStretch(1);
        mv->addLayout(legend);

        memAuto_ = new QCheckBox("内存交给 JVM 自动管理（推荐，不传 -Xmx / -Xms）", w);
        memAuto_->setChecked(cfg_.autoMemory);
        mv->addWidget(memAuto_);

        auto* srow = new QHBoxLayout();
        memSlider_ = new QSlider(Qt::Horizontal, w);
        memSlider_->setRange(512, maxMemoryMb_); memSlider_->setSingleStep(256);
        memSpin_ = new QSpinBox(w); memSpin_->setRange(512, maxMemoryMb_); memSpin_->setSingleStep(256);
        memSpin_->setSuffix(" MB");
        srow->addWidget(new QLabel("分给游戏", w));
        srow->addWidget(memSlider_, 1); srow->addWidget(memSpin_);
        mv->addLayout(srow);
        memPreview_ = new QLabel("-Xmx4096M", w);
        mv->addWidget(memPreview_);
        memWarn_ = new QLabel(w);
        memWarn_->setWordWrap(true);
        memWarn_->hide();
        mv->addWidget(memWarn_);
        xmsCheck_ = new QCheckBox("同时设置初始内存 -Xms", w);
        xmsSpin_ = new QSpinBox(w); xmsSpin_->setRange(256, maxMemoryMb_); xmsSpin_->setSuffix(" MB");
        auto* xrow = new QHBoxLayout(); xrow->addWidget(xmsCheck_); xrow->addWidget(xmsSpin_); xrow->addStretch(1);
        mv->addLayout(xrow);
        v->addWidget(mbox);
        auto* gbox = new QGroupBox("其他", w);
        auto* form = new QFormLayout(gbox);
        setCardTitle(gbox, form, "其他");
        extraArgsEdit_ = new QLineEdit(w);
        extraArgsEdit_->setPlaceholderText("例如 -XX:+UseZGC，空格分隔");
        form->addRow("额外 JVM 参数", extraArgsEdit_);
        langCombo_ = new QComboBox(w);
        langCombo_->addItem(QString("跟随系统（%1）")
                                .arg(QString::fromStdString(core::detectSystemLanguage())), QString());
        for (const char* code : {"en_us", "zh_cn", "zh_tw", "ja_jp", "ko_kr", "de_de",
                                  "fr_fr", "es_es", "pt_br", "ru_ru"})
            langCombo_->addItem(QString::fromLatin1(code), QString::fromLatin1(code));
        form->addRow("游戏语言", langCombo_);
        v->addWidget(gbox);

        auto* abox = new QGroupBox("外观", w);
        auto* aform = new QFormLayout(abox);
        setCardTitle(abox, aform, "外观");
        themeCombo_ = new QComboBox(w);
        themeCombo_->addItem("自动（找到 Noctalia 色板就用）", "auto");
        themeCombo_->addItem("跟随系统调色板", "system");
        themeCombo_->addItem("Noctalia / Material You", "noctalia");
        aform->addRow("配色来源", themeCombo_);
        themeHint_ = new QLabel(w);
        themeHint_->setWordWrap(true);
        themeHint_->setObjectName("pageSubtitle");
        aform->addRow("", themeHint_);
        v->addWidget(abox);
        v->addWidget(aboutSection());
        v->addStretch(1);
        connect(memAuto_, &QCheckBox::toggled, this, [this] {
            const bool manual = !memAuto_->isChecked();
            memSlider_->setEnabled(manual);
            memSpin_->setEnabled(manual);
            xmsCheck_->setEnabled(manual);
            xmsSpin_->setEnabled(manual && xmsCheck_->isChecked());
            updateMemPreview();
            scheduleAutoSave();
        });
        connect(memSlider_, &QSlider::valueChanged, memSpin_, &QSpinBox::setValue);
        connect(memSpin_, QOverload<int>::of(&QSpinBox::valueChanged), memSlider_, &QSlider::setValue);
        connect(memSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
            if (xmsSpin_) xmsSpin_->setMaximum(memSpin_->value());
            updateMemPreview();
        });
        connect(refreshMem, &QPushButton::clicked, this, &LauncherWindow::refreshMemory);
        // 定时器只读 /proc/meminfo（几 KB，微秒级），不占后台线程
        auto* memTimer = new QTimer(this);
        connect(memTimer, &QTimer::timeout, this, &LauncherWindow::refreshMemory);
        memTimer->start(2000);
        refreshMemory();
        return w;
    }

    void refreshLoaderVersions() {
        const std::string loader = loaderCombo_->currentData().toString().toStdString();
        loaderState_->setText("获取版本中…");
        auto cache = layout_.configDir / "cache";
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, loader, cache] {
            std::vector<core::LoaderVersion> list;
            std::string err;
            const bool ok = core::loaderVersions(loader, cache, list, err);
            QMetaObject::invokeMethod(qApp, [guard, ok, err, list] {
                if (guard) guard->applyLoaderVersions(ok, err, list);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void applyLoaderVersions(bool ok, const std::string& err,
                             const std::vector<core::LoaderVersion>& list) {
        loaderVerCombo_->clear();
        if (!ok) {
            loaderState_->setText("获取失败：" + QString::fromStdString(err));
            return;
        }
        for (const auto& v : list)
            loaderVerCombo_->addItem(QString::fromStdString(v.version) + (v.stable ? "（稳定）" : ""),
                                     QString::fromStdString(v.version));
        loaderState_->setText(QString("共 %1 个版本，选一个再点安装").arg(list.size()));
    }

    void installSelectedLoader() {
        if (loaderVerCombo_->count() == 0) { loaderState_->setText("先点「获取版本」"); return; }
        const std::string loader = loaderCombo_->currentData().toString().toStdString();
        const std::string lv = loaderVerCombo_->currentData().toString().toStdString();
        core::Config c = cfg_;
        core::Layout l = layout_;
        installLoaderBtn_->setEnabled(false);
        loaderState_->setText("安装中…（会下载加载器依赖）");
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, l, c, loader, lv]() mutable {
            std::string err;
            const bool ok = core::installLoader(l, c, loader, lv, nullptr, err);
            QMetaObject::invokeMethod(qApp, [guard, ok, err, c, loader, lv] {
                if (!guard) return;
                guard->installLoaderBtn_->setEnabled(true);
                if (!ok) {
                    guard->loaderState_->setText("失败：" + QString::fromStdString(err));
                    return;
                }
                guard->cfg_ = c;
                std::string serr;
                guard->cfg_.save(guard->layout_.configFile, serr);
                guard->loaderState_->setText(QString("已装 %1 %2 → %3")
                                                 .arg(QString::fromStdString(loader), QString::fromStdString(lv),
                                                      QString::fromStdString(core::versionDirName(c))));
                guard->modLog_->appendPlainText(QString("[%1] 加载器 %2 %3 安装完成")
                                                    .arg(QTime::currentTime().toString("HH:mm:ss"),
                                                         QString::fromStdString(loader), QString::fromStdString(lv)));
            }, Qt::QueuedConnection);
        }).detach();
    }

    void refreshGameDirs() {
        if (!dirsTable_) return;
        const auto dirs = core::scanGameDirs(layout_, cfg_.gameDir);
        dirsTable_->setRowCount(0);
        int withVer = 0;
        for (const auto& d : dirs) {
            if (d.versions > 0) withVer++;
            const int r = dirsTable_->rowCount();
            dirsTable_->insertRow(r);
            auto* p0 = new QTableWidgetItem(QString::fromStdString(d.path.string()));
            p0->setData(Qt::UserRole, QString::fromStdString(d.path.string()));
            dirsTable_->setItem(r, 0, p0);
            dirsTable_->setItem(r, 1, new QTableWidgetItem(d.exists ? QString::number(d.versions) : QString("—")));
            dirsTable_->setItem(r, 2, new QTableWidgetItem(d.exists ? QString::number(d.mods) : QString("—")));
            dirsTable_->setItem(r, 3, new QTableWidgetItem(d.current ? "使用中" : ""));
            if (d.current) dirsTable_->selectRow(r);
        }
        dirsState_->setText(withVer > 1
                                ? QString("有 %1 个目录存着游戏版本，可能重复；选中后点「使用所选目录」统一").arg(withVer)
                                : QString("只有 %1 个目录存着游戏版本").arg(withVer));
    }

    void applySelectedGameDir() {
        const int r = dirsTable_->currentRow();
        if (r < 0) { dirsState_->setText("先在表里选一个目录"); return; }
        gameDirEdit_->setText(dirsTable_->item(r, 0)->data(Qt::UserRole).toString());
        collectUiToConfig();
        saveConfigQuietly();
        refreshGameDirs();
        refreshLocalVersions();
    }

    void checkGameDirs() {
        refreshGameDirs();
        if (!dirsTable_) return;
        int withVer = 0;
        for (int i = 0; i < dirsTable_->rowCount(); ++i) {
            const QString v = dirsTable_->item(i, 1) ? dirsTable_->item(i, 1)->text() : QString();
            if (!v.isEmpty() && v != "—" && v != "0") withVer++;
        }
        if (withVer > 1)
            statusBar()->showMessage(
                QString("检测到 %1 个目录存有游戏版本，可在「游戏目录」页统一").arg(withVer), 12000);
    }

    // 从应用目录往上找 README.md（源码树里 exe 在 build/，上一级就是仓库根）
    static QString findLocalReadme(const core::Layout& l) {
        std::filesystem::path p = l.appDir;
        for (int i = 0; i < 4; ++i) {
            std::filesystem::path cand = p / "README.md";
            if (mc263::fsx::isFile(cand)) return QString::fromStdString(cand.string());
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
        return {};
    }

    QWidget* aboutSection() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QString linkColor = dark ? QStringLiteral("#7cc0ff") : QStringLiteral("#1a5fd0");

        v->setContentsMargins(0, 10, 0, 0);

        QString text =
            "本项目源码许可：GPL-3.0-or-later（见 LICENSE）\n"
            "Qt6 运行时：LGPL-3.0-only OR GPL-3.0-only WITH Qt-GPL-exception-1.0（动态链接）\n"
            "游戏文件、Mojang Java 运行时与第三方库各自适用其原有许可，本工具不随仓库分发它们。";
        auto* licBox = new QGroupBox("许可与说明", w);
        auto* licLayout = new QVBoxLayout(licBox);
        setCardTitle(licBox, licLayout, "许可与说明");
        auto* label = new QLabel(text, licBox);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        licLayout->addWidget(label);
        v->addWidget(licBox);
        v->addSpacing(4);

        // 本地 README：蓝色可点，用系统默认程序打开（Wayland 下走 xdg-open / portal）
        const QString readme = findLocalReadme(layout_);
        auto* docRow = new QHBoxLayout();
        docRow->addWidget(new QLabel("说明文档：", w));
        if (readme.isEmpty()) {
            auto* miss = new QLabel("未找到 README.md（已从应用目录向上找 4 层）", w);
            miss->setStyleSheet("color:" + (dark ? QStringLiteral("#9aa4b2")
                                                  : QStringLiteral("#5b6472")) + ";");
            docRow->addWidget(miss, 1);
        } else {
            auto* docLink = new QLabel(w);
            docLink->setTextFormat(Qt::RichText);
            docLink->setText(QStringLiteral("<a href=\"%1\" style=\"color:%2;\">README.md（本地）</a>")
                                 .arg(QUrl::fromLocalFile(readme).toString(), linkColor));
            docLink->setOpenExternalLinks(true);
            docLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
            docLink->setCursor(Qt::PointingHandCursor);
            docLink->setToolTip(readme);
            docRow->addWidget(docLink, 1);
        }
        v->addLayout(docRow);

        // 仓库链接：蓝字、可点击、用桌面默认浏览器打开（Wayland 下走 xdg-open / portal）。
        // 蓝色随深浅色主题切换，避免深蓝在深色底上糊成一片。
        auto* repoRow = new QHBoxLayout();
        auto* link = new QLabel(w);
        link->setTextFormat(Qt::RichText);
        link->setText(QStringLiteral("<a href=\"%1\" style=\"color:%2;\">%1</a>")
                          .arg(QString::fromLatin1(kRepoUrl), linkColor));
        link->setOpenExternalLinks(true);
        link->setTextInteractionFlags(Qt::TextBrowserInteraction);
        link->setCursor(Qt::PointingHandCursor);
        link->setToolTip("点击用默认浏览器打开 GitHub 仓库，也可以选中后复制");
        repoRow->addWidget(new QLabel("仓库：", w));
        repoRow->addWidget(link, 1);
        v->addLayout(repoRow);

        // 实际生效的显示后端（Wayland 原生 / XWayland / offscreen），方便排查
        const QString plat = QGuiApplication::platformName();
        QString platNote = plat;
        if (plat.startsWith("wayland")) platNote += "（原生 Wayland，不经过 XWayland）";
        else if (plat == "xcb") platNote += "（X11 / XWayland 转发）";
        auto* platLabel = new QLabel("显示后端：" + platNote + "　·　Qt " +
                                         QString::fromLatin1(qVersion()), w);
        platLabel->setStyleSheet("color:" +
                                 (dark ? QStringLiteral("#9aa4b2") : QStringLiteral("#5b6472")) + ";");
        v->addWidget(platLabel);
        return w;
    }

    // 读取 /proc/meminfo，刷新「其他程序占用 / 缓存 / 仅可用」
    void refreshMemory() {
        core::MemInfo mi = core::readMemInfo();
        if (mi.totalMb == 0) return;  // 读不到就保留上一次数值
        memInfo_ = mi;
        if (memTotalMb_ == 0) memTotalMb_ = static_cast<int>(mi.totalMb);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const MemColors mc = memColors(palette());
        const QString cUsed = mc.used.name();
        const QString cCache = mc.cache.name();
        const QString cAvail = dark ? QStringLiteral("#9aa4b2") : QStringLiteral("#5b6472");
        const int pct = mi.totalMb ? static_cast<int>(mi.usedMb * 100 / mi.totalMb) : 0;
        memSummary_->setText(QString("物理内存 <b>%1</b> · 其他程序占用 <b>%2</b>（%3%）")
                                 .arg(fmtMem(mi.totalMb), fmtMem(mi.usedMb)).arg(pct));
        usedStat_->setText(QString("<span style='color:%1'>■</span> 程序占用 <b>%2</b>")
                               .arg(cUsed, fmtMem(mi.usedMb)));
        cacheStat_->setText(QString("<span style='color:%1'>■</span> 缓存 <b>%2</b>")
                                .arg(cCache, fmtMem(mi.cachedMb)));
        availStat_->setText(QString("<span style='color:%1'>■</span> 仅可用 <b>%2</b>")
                                .arg(cAvail, fmtMem(mi.availableMb)));
        updateMemPreview();
    }

    void updateMemPreview() {
        if (!memPreview_) return;
        const bool autoMode = memAuto_ && memAuto_->isChecked();
        const int want = autoMode
                             ? static_cast<int>(std::max<uint64_t>(1024, memInfo_.totalMb / 4))
                             : (memSpin_ ? memSpin_->value() : 0);
        if (xmsSpin_) xmsSpin_->setMaximum(std::max(256, want));
        const uint64_t total = memInfo_.totalMb;
        const uint64_t avail = memInfo_.availableMb;
        const int pTotal = total ? static_cast<int>(want * 100 / total) : 0;
        const int pAvail = avail ? static_cast<int>(want * 100 / avail) : 0;
        QString line = autoMode
                           ? QString("JVM 自动：预计上限约 %1（不传 -Xmx）").arg(fmtMem(static_cast<uint64_t>(want)))
                           : QString("-Xmx%1M　·　占物理内存 %2%　·　占当前可用 %3%")
                                 .arg(want).arg(pTotal).arg(pAvail);
        if (avail) {
            const uint64_t left = static_cast<uint64_t>(want) < avail
                                      ? avail - static_cast<uint64_t>(want) : 0;
            line += "　·　启动后预计剩余 " + fmtMem(left);
        }
        memPreview_->setText(line);
        if (gameStat_) {
            const MemColors mc = memColors(palette());
            gameStat_->setText(autoMode
                                   ? QString("<span style='color:%1'>■</span> 游戏 -Xmx <b>JVM 自动</b>")
                                         .arg(mc.game.name())
                                   : QString("<span style='color:%1'>■</span> 游戏 -Xmx <b>%2</b>")
                                         .arg(mc.game.name(), fmtMem(static_cast<uint64_t>(want))));
        }
        if (memBar_) memBar_->setStats(memInfo_, want);
        if (!memWarn_) return;
        if (autoMode) { memWarn_->hide(); return; }
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        if (avail && static_cast<uint64_t>(want) > avail) {
            memWarn_->setText(QString("注意：超出当前可用内存 %1（仅可用 %2），启动后可能被系统 OOM 杀掉")
                                  .arg(fmtMem(static_cast<uint64_t>(want) - avail), fmtMem(avail)));
            memWarn_->setStyleSheet(QString("color:%1;").arg(dark ? "#f87171" : "#b91c1c"));
            memWarn_->show();
        } else {
            memWarn_->hide();
        }
    }

    // 切页：换索引 + 旧页淡出。动画时长写在这里，想关掉把 150 改成 0。
    // 不用 QGraphicsOpacityEffect：窗口是 WA_TranslucentBackground，effect 会把自绘的
    // 窗口底色一起淡成透明，看起来就是"整块变透明/动画失败"。改成把旧页抓成一张
    // 透明度递减的贴图盖在新页上，底色始终由 paintEvent 画，不会打洞。
    void switchPage(int idx) {
        if (!stack_ || idx < 0 || idx >= stack_->count()) return;
        if (idx == stack_->currentIndex()) return;
        if (pageAnim_) { pageAnim_->stop(); pageAnim_->deleteLater(); pageAnim_ = nullptr; }
        if (fadeOverlay_) {
            fadeOverlay_->hide();          // 先藏起来，否则下一次 grab 会把上一张残影也抓进去
            fadeOverlay_->deleteLater();
            fadeOverlay_ = nullptr;
        }
        if (switching_) return;            // 极端情况下（重入）直接跳过这次动画
        switching_ = true;
        const QPixmap old = stack_->grab();
        stack_->setCurrentIndex(idx);
        if (old.isNull()) return;
        auto* ov = new FadeOverlay(stack_, old);
        fadeOverlay_ = ov;
        auto* anim = new QVariantAnimation(this);
        anim->setStartValue(0.0);
        anim->setEndValue(1.0);
        anim->setDuration(150);
        anim->setEasingCurve(QEasingCurve::OutQuad);
        connect(anim, &QVariantAnimation::valueChanged, ov,
                [ov](const QVariant& v) { ov->setFade(1.0 - v.toReal()); });
        connect(anim, &QVariantAnimation::finished, this, [this, ov, anim] {
            if (fadeOverlay_ == ov) fadeOverlay_ = nullptr;
            ov->deleteLater();
            anim->deleteLater();
        });
        pageAnim_ = anim;
        anim->start();
        switching_ = false;
    }

    // ---- 配色来源：系统调色板 / Noctalia（Material You）色板 ----
    // niri 下没有系统级的强调色概念，Noctalia 会把壁纸算出来的 M3 色板渲染成
    // 一份扁平 TOML（本机 ~/.cache/nyxuri/palette.toml），启动器直接读它。
    void applyPalette() {
        if (!qApp) return;
        m3_ = core::findM3Palette();
        const std::string src = cfg_.themeSource;
        const bool useNoctalia =
            m3_.valid() && (src == "noctalia" || (src == "auto" && !m3_.path.empty()));
        if (useNoctalia)
            qApp->setPalette(paletteFromM3(m3_));
        else
            qApp->setPalette(defaultPalette_);
        updateThemeHint();
        setupPaletteWatch();
    }

    void applyStyleSheet() {
        if (!qApp) return;
        const Theme th = themeFor(qApp->palette());
        theme_ = th;
        if (roundedCorners_) update();   // 背景是自绘的，换色后要重画
        if (qEnvironmentVariableIntValue("MC263_DEBUG_THEME") == 1)
            std::fprintf(stderr, "mc263-qt: 配色=%s 色板=%s window=%s accent=%s\n",
                         cfg_.themeSource.c_str(),
                         m3_.valid() ? m3_.path.c_str() : "(系统调色板)",
                         th.window.toUtf8().constData(), th.accent.toUtf8().constData());
        const QString chevron = makeChevron(QString::fromStdString(layout_.configDir.string()),
                                            QColor(th.muted));
        qApp->setStyleSheet(buildStyleSheet(th, chevron));
        updateThemeHint();
    }

    void updateThemeHint() {
        if (!themeHint_) return;
        QString txt;
        if (m3_.valid())
            txt = QStringLiteral("读的是 %1（%2）")
                      .arg(QString::fromStdString(m3_.path),
                           m3_.dark() ? QStringLiteral("深色") : QStringLiteral("浅色"));
        else
            txt = QStringLiteral("没找到 Noctalia 色板（会依次找 %1）")
                      .arg(QString::fromStdString(core::paletteCandidates().front()));
        if (core::isNiriSession()) txt += QStringLiteral(" · niri 会话");
        themeHint_->setText(txt);
    }

    // 壁纸一换 Noctalia 会重渲染色板：盯着文件，变了就重新上色（不用重启）
    void setupPaletteWatch() {
        if (!paletteWatch_) {
            paletteWatch_ = new QFileSystemWatcher(this);
            connect(paletteWatch_, &QFileSystemWatcher::fileChanged, this, [this](const QString&) {
                applyPalette();
                applyStyleSheet();
            });
        }
        const QStringList watched = paletteWatch_->files();
        if (!watched.isEmpty()) paletteWatch_->removePaths(watched);
        if (m3_.valid())
            paletteWatch_->addPath(QString::fromStdString(m3_.path));
    }

    void loadConfigToUi() {
        usernameEdit_->setText(QString::fromStdString(cfg_.username));
        launchVersion_->setCurrentText(QString::fromStdString(cfg_.version));
        downloadVersion_->setCurrentText(QString::fromStdString(cfg_.version));
        int mi = mirrorCombo_->findData(QString::fromStdString(cfg_.mirror));
        if (mi >= 0) mirrorCombo_->setCurrentIndex(mi);
        jobsSpin_->setValue(cfg_.jobs);
        for (auto* cb : compChecks_)
            cb->setChecked(std::find(cfg_.components.begin(), cfg_.components.end(),
                                    cb->property("comp").toString().toStdString()) != cfg_.components.end());
        if (compChecks_.size() >= 3 && cfg_.components.empty())
            for (auto* cb : compChecks_) cb->setChecked(true);
        if (runtimeCheck_)
            runtimeCheck_->setChecked(std::find(cfg_.components.begin(), cfg_.components.end(),
                                               "runtime") != cfg_.components.end());
        gameDirEdit_->setText(QString::fromStdString(layout_.gameDir.string()));
        for (const auto& a : cfg_.extraJvmArgs) {
            if (!extraArgsEdit_->text().isEmpty()) extraArgsEdit_->setText(extraArgsEdit_->text() + " ");
            extraArgsEdit_->setText(extraArgsEdit_->text() + QString::fromStdString(a));
        }
        if (themeCombo_) {
            const int ti = themeCombo_->findData(QString::fromStdString(cfg_.themeSource));
            themeCombo_->setCurrentIndex(ti >= 0 ? ti : 0);
        }
        if (langCombo_) {
            const int li = langCombo_->findData(QString::fromStdString(cfg_.language));
            langCombo_->setCurrentIndex(li >= 0 ? li : 0);
        }
        memAuto_->setChecked(cfg_.autoMemory);
        memSlider_->setEnabled(!cfg_.autoMemory);
        memSpin_->setEnabled(!cfg_.autoMemory);
        xmsCheck_->setEnabled(!cfg_.autoMemory);
        xmsSpin_->setEnabled(!cfg_.autoMemory && cfg_.minMemoryMb > 0);
        memSpin_->setValue(std::min(cfg_.memoryMb, maxMemoryMb_));
        memSlider_->setValue(memSpin_->value());
        xmsCheck_->setChecked(cfg_.minMemoryMb > 0);
        xmsSpin_->setValue(cfg_.minMemoryMb > 0 ? cfg_.minMemoryMb : 512);
        updateMemPreview();
    }
    void collectUiToConfig() {
        cfg_.version = launchVersion_->currentText().toStdString();
        if (cfg_.version.empty()) cfg_.version = "26.3";
        cfg_.username = usernameEdit_->text().toStdString();
        cfg_.mirror = mirrorCombo_->currentData().toString().toStdString();
        cfg_.jobs = jobsSpin_->value();
        cfg_.arch = core::detectArch();  // 架构不再让用户选，按本机自动判断
        cfg_.autoMemory = memAuto_->isChecked();
        cfg_.memoryMb = memSpin_->value();
        cfg_.minMemoryMb = xmsCheck_->isChecked() ? xmsSpin_->value() : 0;
        cfg_.components.clear();
        for (auto* cb : compChecks_) if (cb->isChecked()) cfg_.components.push_back(cb->property("comp").toString().toStdString());
        if (runtimeCheck_ && runtimeCheck_->isVisible() && runtimeCheck_->isChecked())
            cfg_.components.push_back("runtime");
        if (langCombo_) cfg_.language = langCombo_->currentData().toString().toStdString();
        if (themeCombo_) cfg_.themeSource = themeCombo_->currentData().toString().toStdString();
        cfg_.extraJvmArgs.clear();
        QString extra = extraArgsEdit_->text().trimmed();
        if (!extra.isEmpty()) for (const QString& s : extra.split(' ', Qt::SkipEmptyParts))
            cfg_.extraJvmArgs.push_back(s.toStdString());
        // 注意：这里必须用 dataRoot_ 当沙箱根。装到 /usr/bin 时 exe 目录不可写、
        // 数据在 ~/.local/share/mc263，用 Layout::detect(exe, dir) 会把沙箱根设成
        // /usr/bin，于是 XDG 下的目录被守卫当成 /home 敏感路径拒掉（启动直接报
        // "无法创建运行目录: 拒绝写入受保护目录"）。
        std::string dir = gameDirEdit_->text().trimmed().toStdString();
        if (dir.empty()) {
            cfg_.gameDir.clear();
            layout_ = core::Layout::at(dataRoot_);
        } else if (dir != layout_.gameDir.string()) {
            layout_ = core::Layout::at(dataRoot_, dir);
            cfg_.gameDir = dir;
        }
        // 兜底：万一哪条路径又把沙箱根搞坏了，宁可退回数据根，也别让启动时报
        // "拒绝写入受保护目录" 这种用户完全没法处理的错。
        std::string lerr;
        if (!layout_.writable(lerr)) layout_ = core::Layout::at(dataRoot_);
    }

    // ---- 自动保存：任何设置改动 400ms 后写盘，不需要「保存」按钮 ----
    void setupAutoSave() {
        autosaveTimer_ = new QTimer(this);
        autosaveTimer_->setSingleShot(true);
        autosaveTimer_->setInterval(400);
        connect(autosaveTimer_, &QTimer::timeout, this, &LauncherWindow::saveConfigQuietly);

        connect(usernameEdit_, &QLineEdit::textChanged, this, &LauncherWindow::scheduleAutoSave);
        connect(extraArgsEdit_, &QLineEdit::textChanged, this, &LauncherWindow::scheduleAutoSave);
        connect(launchVersion_, &QComboBox::currentTextChanged, this, &LauncherWindow::scheduleAutoSave);
        connect(mirrorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &LauncherWindow::scheduleAutoSave);
        connect(jobsSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &LauncherWindow::scheduleAutoSave);
        connect(memSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &LauncherWindow::scheduleAutoSave);
        connect(xmsSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &LauncherWindow::scheduleAutoSave);
        connect(langCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &LauncherWindow::scheduleAutoSave);
        connect(themeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
            if (loadingConfig_) return;
            cfg_.themeSource = themeCombo_->currentData().toString().toStdString();
            applyPalette();
            applyStyleSheet();
            scheduleAutoSave();
        });
        connect(xmsCheck_, &QCheckBox::toggled, this, &LauncherWindow::scheduleAutoSave);
        for (auto* cb : compChecks_)
            connect(cb, &QCheckBox::toggled, this, &LauncherWindow::scheduleAutoSave);
        connect(runtimeCheck_, &QCheckBox::toggled, this, &LauncherWindow::scheduleAutoSave);
        // 游戏目录手输容易半截，等编辑结束再存
        connect(gameDirEdit_, &QLineEdit::editingFinished, this, &LauncherWindow::scheduleAutoSave);
    }

    void scheduleAutoSave() {
        if (loadingConfig_ || !autosaveTimer_) return;
        autosaveTimer_->start();
    }

    void saveConfigQuietly() {
        if (loadingConfig_) return;
        collectUiToConfig();
        std::string err;
        if (!cfg_.save(layout_.configFile, err)) {
            statusBar()->showMessage("配置保存失败：" + QString::fromStdString(err), 8000);
            return;
        }
        statusBar()->showMessage("已自动保存 " + QTime::currentTime().toString("HH:mm:ss") +
                                 " · " + QString::fromStdString(layout_.configFile.string()), 3000);
    }

    void refreshLocalVersions() {
        auto vers = core::availableVersions(layout_);
        QString cur = launchVersion_->currentText();
        launchVersion_->clear();
        for (const auto& v : vers) launchVersion_->addItem(QString::fromStdString(v));
        if (cur.isEmpty() && !cfg_.version.empty()) launchVersion_->addItem(QString::fromStdString(cfg_.version));
        if (!cur.isEmpty()) launchVersion_->setCurrentText(cur);
        else launchVersion_->setCurrentText(QString::fromStdString(cfg_.version));
        launchState_->setText("本地版本 " + QString::number(vers.size()) + " 个");
    }
    void refreshOnlineVersions() {
        auto* p = new QProcess(this);
        p->setProgram(pythonBin());
        p->setArguments({QString::fromStdString(scriptPath_.string()), "--list-versions",
                         "--mc-dir", QString::fromStdString(layout_.gameDir.string())});
        QPointer<LauncherWindow> guard(this);
        connect(p, &QProcess::finished, this, [this, guard, p](int, QProcess::ExitStatus) {
            QByteArray out = p->readAllStandardOutput();
            std::vector<std::string> vers;
            QJsonDocument doc = QJsonDocument::fromJson(out);
            if (doc.isObject()) {
                QJsonArray arr = doc.object().value("versions").toArray();
                for (const auto& v : arr) vers.push_back(v.toString().toStdString());
            }
            if (guard) applyOnlineVersions(vers, vers.empty() ? QString::fromUtf8(p->readAllStandardError()) : QString());
            p->deleteLater();
        });
        p->start();
    }

    void refreshJava() {
        javaState_->setText("后台扫描 Java 中…");
        core::Layout l = layout_;
        core::Config c = cfg_;
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, l, c] {
            auto list = core::findJava(l, c);
            QMetaObject::invokeMethod(qApp, [guard, list] {
                if (guard) guard->applyJavaList(list);
            }, Qt::QueuedConnection);
        }).detach();
    }
    void runCompat() {
        compatState_->setText("后台查验中…");
        core::Layout l = layout_;
        core::Config c = cfg_;
        auto javas = javas_;
        bool net = netCheck_->isChecked();
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, l, c, javas, net] {
            auto items = core::compatibility(l, c, javas, net);
            QMetaObject::invokeMethod(qApp, [guard, items] {
                if (guard) guard->applyCompat(items);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void clearDownloadCache() {
        const std::filesystem::path cache = layout_.configDir / "cache";
        int files = 0;
        uint64_t bytes = 0;
        dirUsage(cache, files, bytes);
        if (files == 0) {
            downloadLog_->appendPlainText("[" + QTime::currentTime().toString("HH:mm:ss") +
                                         "] 缓存已是空的：" + QString::fromStdString(cache.string()));
            downloadLabel_->setText("缓存为空");
            return;
        }
        const QString msg = QString("将删除 %1 个缓存文件（%2）：\n%3\n\n只清元数据缓存，已下载的游戏文件（jar / libraries / assets）不受影响。")
                                .arg(files)
                                .arg(fmtMem(bytes / (1024 * 1024)))
                                .arg(QString::fromStdString(cache.string()));
        if (QMessageBox::question(this, "清除下载缓存", msg, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
            QMessageBox::Yes)
            return;
        std::error_code ec;
        std::filesystem::remove_all(cache, ec);
        std::filesystem::create_directories(cache, ec);
        if (ec) {
            downloadLog_->appendPlainText("[" + QTime::currentTime().toString("HH:mm:ss") +
                                         "] 清除失败：" + QString::fromStdString(ec.message()));
            return;
        }
        downloadLog_->appendPlainText(QString("[%1] 已清除下载缓存：%2 个文件，%3")
                                          .arg(QTime::currentTime().toString("HH:mm:ss"))
                                          .arg(files)
                                          .arg(fmtMem(bytes / (1024 * 1024))));
        downloadLabel_->setText(QString("缓存已清除（%1）").arg(fmtMem(bytes / (1024 * 1024))));
    }

    void startDownload() {
        if (downloadProc_ && downloadProc_->state() != QProcess::NotRunning) {
            QMessageBox::information(this, "正在下载", "下载任务已经在运行");
            return;
        }
        collectUiToConfig();
        std::string err;
        cfg_.save(layout_.configFile, err);
        saveDownloaderScript();
        downloadLog_->clear();
        downloadLog_->appendPlainText("启动 python3 下载器（多线程并发 " + QString::number(cfg_.jobs) + "）…");
        QStringList comps;
        for (const auto& s : cfg_.components) comps << QString::fromStdString(s);
        downloadProc_ = new QProcess(this);
        downloadProc_->setProcessChannelMode(QProcess::SeparateChannels);
        downloadProc_->setProgram(pythonBin());
        downloadProc_->setArguments({QString::fromStdString(scriptPath_.string()),
                                     "--mc-dir", QString::fromStdString(layout_.gameDir.string()),
                                     "--version", downloadVersion_->currentText(),
                                     "--mirror", QString::fromStdString(cfg_.mirror),
                                     "--jobs", QString::number(cfg_.jobs),
                                     "--arch", QString::fromStdString(cfg_.arch),
                                     "--components", comps.join(',')});
        connect(downloadProc_, &QProcess::readyReadStandardOutput, this, &LauncherWindow::onDownloadOutput);
        connect(downloadProc_, &QProcess::readyReadStandardError, this, [this] {
            downloadLog_->appendPlainText(QString::fromUtf8(downloadProc_->readAllStandardError()).trimmed());
        });
        connect(downloadProc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            downloadBtn_->setEnabled(true);
            cancelBtn_->setEnabled(false);
            downloadLabel_->setText(code == 0 ? "下载完成" : "下载结束（退出码 " + QString::number(code) + "）");
            if (code == 0) {
                refreshLocalVersions();
                refreshJava();  // 运行时下载完可能就有 Java 25 了，重新过一遍闸门
            }
        });
        downloadBtn_->setEnabled(false);
        cancelBtn_->setEnabled(true);
        downloadProc_->start();
    }
    void onDownloadOutput() {
        buffer_ += QString::fromUtf8(downloadProc_->readAllStandardOutput());
        int idx;
        while ((idx = buffer_.indexOf('\n')) >= 0) {
            QString line = buffer_.left(idx).trimmed();
            buffer_.remove(0, idx + 1);
            if (line.isEmpty()) continue;
            QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
            if (!doc.isObject()) { downloadLog_->appendPlainText(line); continue; }
            QJsonObject o = doc.object();
            QString stage = o.value("stage").toString();
            int done = o.value("done").toInt(), total = o.value("total").toInt();
            if (stage == "done") {
                downloadLog_->appendPlainText(o.value("ok").toBool() ? "全部完成" : "部分文件失败");
                continue;
            }
            if (total > 0) {
                downloadBar_->setRange(0, total);
                downloadBar_->setValue(done);
            }
            downloadLabel_->setText(stage + "  " + QString::number(done) + "/" + QString::number(total) +
                                    "  " + o.value("label").toString());
            if (done % 50 == 0 || !o.value("ok").toBool())
                downloadLog_->appendPlainText(stage + " " + QString::number(done) + "/" +
                                              QString::number(total) + " " + o.value("label").toString() +
                                              (o.value("ok").toBool() ? "" : " 失败: " + o.value("note").toString()));
        }
    }

    void startLaunch() {
        if (launchProc_ && launchProc_->state() != QProcess::NotRunning) {
            QMessageBox::information(this, "正在运行", "游戏已经在运行");
            return;
        }
        collectUiToConfig();
        // 启动前再确认一次：阈值按当前版本算（26.x=25、1.20+=21、1.17+=17），
        // 不达标就把闸门页拉回来，别让用户点了个寂寞
        requiredJava_ = core::requiredJavaMajor(layout_, cfg_.version);
        int bestMajor = 0;
        for (const auto& j : javas_)
            if (j.valid) bestMajor = std::max(bestMajor, j.major);
        javaUsable_ = javaScanDone_ && bestMajor >= requiredJava_;
        if (!javaUsable_) {
            if (!javaScanDone_) refreshJava();
            applyJavaGate();
            return;
        }
        core::JavaInfo java;
        if (!cfg_.javaPath.empty()) {
            java.path = cfg_.javaPath;
            java.valid = mc263::fsx::isFile(java.path);
            java.major = 25;
            java.version = "手动指定";
        } else {
            for (const auto& j : javas_) if (j.valid) { java = j; break; }
        }
        core::LaunchPlan plan = core::buildLaunchPlan(layout_, cfg_, java);
        if (!plan.ok()) {
            QMessageBox::warning(this, "无法启动", QString::fromStdString(plan.error));
            return;
        }
        launchLog_->clear();
        launchLog_->appendPlainText("Java: " + QString::fromStdString(java.path.string()));
        launchLog_->appendPlainText("主类: " + QString::fromStdString(plan.mainClass));
        launchLog_->appendPlainText("classpath " + QString::number(plan.argv.size()) + " 个参数");
        launchProc_ = new QProcess(this);
        launchProc_->setProcessChannelMode(QProcess::MergedChannels);
        launchProc_->setWorkingDirectory(QString::fromStdString(plan.workDir.string()));
        QStringList args;
        for (size_t i = 1; i < plan.argv.size(); ++i) args << QString::fromStdString(plan.argv[i]);
        launchProc_->setProgram(QString::fromStdString(plan.argv[0]));
        launchProc_->setArguments(args);
        connect(launchProc_, &QProcess::readyReadStandardOutput, this, [this] {
            launchLog_->appendPlainText(QString::fromUtf8(launchProc_->readAllStandardOutput()).trimmed());
        });
        connect(launchProc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            launchState_->setText("游戏已退出（" + QString::number(code) + "）");
        });
        launchState_->setText("运行中…");
        launchProc_->start();
    }

    core::Layout layout_;
    std::filesystem::path dataRoot_;
    core::Config cfg_;
    std::vector<core::JavaInfo> javas_;
    std::filesystem::path scriptPath_;
    int maxMemoryMb_ = 8192;
    QString buffer_;
    QProcess* downloadProc_ = nullptr;
    QProcess* launchProc_ = nullptr;
    QTabBar* nav_ = nullptr;
    QPointer<QWidget> fadeOverlay_;
    QPointer<QVariantAnimation> pageAnim_;
    bool switching_ = false;
    QStackedWidget* stack_ = nullptr;
    QStackedWidget* rootStack_ = nullptr;
    QWidget* mainPage_ = nullptr;
    QWidget* gatePage_ = nullptr;
    QLabel* gateTitle_ = nullptr;
    QLabel* gateStatus_ = nullptr;
    QLabel* gateDetail_ = nullptr;
    QPushButton* gateDownloadBtn_ = nullptr;
    QCheckBox* runtimeCheck_ = nullptr;
    bool javaScanDone_ = false;
    bool javaUsable_ = false;
    int javaOldMajor_ = 0;
    int requiredJava_ = 25;  // 随版本变化：26.x=25、1.20+=21、1.17+=17
    std::string javaOldVersion_;
    QComboBox* launchVersion_ = nullptr;
    QLineEdit* usernameEdit_ = nullptr;
    QPushButton* launchBtn_ = nullptr;
    QLabel* launchState_ = nullptr;
    QPlainTextEdit* launchLog_ = nullptr;
    QComboBox* downloadVersion_ = nullptr;
    QComboBox* mirrorCombo_ = nullptr;
    QSpinBox* jobsSpin_ = nullptr;
    std::vector<QCheckBox*> compChecks_;
    QPushButton* downloadBtn_ = nullptr;
    QPushButton* clearCacheBtn_ = nullptr;

    QLineEdit* modQuery_ = nullptr;
    QComboBox* modLoader_ = nullptr;
    QComboBox* modSort_ = nullptr;
    QComboBox* loaderCombo_ = nullptr;
    QComboBox* loaderVerCombo_ = nullptr;
    QPushButton* installLoaderBtn_ = nullptr;
    QLabel* loaderState_ = nullptr;
    QLineEdit* modVersionEdit_ = nullptr;
    QTableWidget* modTable_ = nullptr;
    QPushButton* modSearchBtn_ = nullptr;
    QPushButton* modInstallBtn_ = nullptr;
    QLabel* modState_ = nullptr;
    QPlainTextEdit* modLog_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QProgressBar* downloadBar_ = nullptr;
    QLabel* downloadLabel_ = nullptr;
    QPlainTextEdit* downloadLog_ = nullptr;
    QTableWidget* javaTable_ = nullptr;
    QComboBox* javaCombo_ = nullptr;
    QLabel* javaState_ = nullptr;
    QTableWidget* compatTable_ = nullptr;
    QLabel* compatState_ = nullptr;
    QCheckBox* netCheck_ = nullptr;
    QCheckBox* memAuto_ = nullptr;
    QSlider* memSlider_ = nullptr;
    QSpinBox* memSpin_ = nullptr;
    QSpinBox* xmsSpin_ = nullptr;
    QCheckBox* xmsCheck_ = nullptr;
    MemoryBar* memBar_ = nullptr;
    QLabel* memSummary_ = nullptr;
    QLabel* usedStat_ = nullptr;
    QLabel* cacheStat_ = nullptr;
    QLabel* gameStat_ = nullptr;
    QLabel* availStat_ = nullptr;
    QLabel* memWarn_ = nullptr;
    core::MemInfo memInfo_;
    int memTotalMb_ = 0;
    QLabel* memPreview_ = nullptr;
    QLineEdit* gameDirEdit_ = nullptr;
    QPushButton* browseBtn_ = nullptr;
    QTableWidget* dirsTable_ = nullptr;
    QLabel* dirsState_ = nullptr;
    QLineEdit* extraArgsEdit_ = nullptr;
    QComboBox* langCombo_ = nullptr;
    QComboBox* themeCombo_ = nullptr;
    QLabel* themeHint_ = nullptr;
    QFileSystemWatcher* paletteWatch_ = nullptr;
    QPalette defaultPalette_;
    core::M3Palette m3_;
    Theme theme_;
    bool roundedCorners_ = false;
    QTimer* autosaveTimer_ = nullptr;
    bool loadingConfig_ = false;
};

// 崩溃时把调用栈打到 stderr：终端里跑 ./build/mc263-qt 就能看到，方便定位
static void fatalSignal(int sig) {
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    const std::string head = "\nmc263-qt: 收到信号 " + std::to_string(sig) + "，调用栈：\n";
    ::write(2, head.data(), head.size());
    ::backtrace_symbols_fd(frames, n, 2);
    ::_Exit(128 + sig);
}

int main(int argc, char** argv) {
    std::signal(SIGSEGV, fatalSignal);
    std::signal(SIGABRT, fatalSignal);
    std::signal(SIGBUS, fatalSignal);
    // 平台插件必须在 QApplication 之前定好：Wayland 会话优先原生 wayland，
    // 缺插件时回退 xcb（XWayland），两者都没有则直接给出安装提示后退出。
    const core::PlatformPick pick = preparePlatform();
    if (pick.fatal) {
        std::fprintf(stderr,
                     "mc263-qt: %s\n"
                     "  Fedora : sudo dnf install qt6-qtwayland\n"
                     "  openSUSE: sudo zypper install libQt6WaylandClient6\n"
                     "  或在有 X11/XWayland 时指定：QT_QPA_PLATFORM=xcb %s\n",
                     pick.note.c_str(), argv[0]);
        return 1;
    }
    if (pick.plugin == "xcb" && waylandSession())
        std::fprintf(stderr, "mc263-qt: %s\n", pick.note.c_str());
    if (qEnvironmentVariable("MC263_VERBOSE") == "1")
        std::fprintf(stderr, "mc263-qt: %s\n", pick.note.c_str());

    QApplication app(argc, argv);
    app.setApplicationName("mc263");
    app.setApplicationVersion("26.3");
    // Wayland 下窗口的 app_id 由 desktopFileName 决定：与桌面项同名时任务栏图标、
    // 窗口分组、通知归属才正确（X11 用 StartupWMClass）。这里用 mc263-qt，
    // 避免和产物 mc263（游戏本体启动器）的桌面项混在一起。
    QGuiApplication::setDesktopFileName(QStringLiteral("mc263-qt"));
    const QIcon themeIcon = QIcon::fromTheme(QStringLiteral("mc263"));
    if (!themeIcon.isNull()) app.setWindowIcon(themeIcon);
    LauncherWindow win;
    win.show();
    return app.exec();
}
