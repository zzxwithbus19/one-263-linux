// mc263_qt.cpp -- HMCL 式 Qt6 原生启动器（单个 ELF）
//
// 形态：[mc263-qt ELF][appDir/minecraft/ 游戏目录][appDir/.mc263/ 配置]
// 下载：进程内 C++ 下载核心（curl + SHA1 + 并发任务池），后台线程执行；
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
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
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
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QVariantAnimation>
#include <QPushButton>
#include <QSizePolicy>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringListModel>
#include <QTabWidget>
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
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace core = mc263::core;

// 启动器自身崩溃时写的文件。路径在窗口构造里填好，信号处理器只做 open/write。
static char g_crashLogPath[4096] = {0};

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

/* 版本管理左栏：卡片式列表，选中态是实心强调色 */
QListWidget#verList {
    background: @card@; border: 1px solid @border@; border-radius: 10px; padding: 4px;
}
QListWidget#verList::item { height: 38px; padding: 0 10px; border-radius: 8px; color: @text@; }
QListWidget#verList::item:hover { background: @hover@; }
QListWidget#verList::item:selected { background: @accent@; color: white; }

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
    font-size: 13px; font-weight: 500; padding-bottom: 6px;
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

// 表格默认行高偏矮、整体容易被布局压缩成一条，这里统一给个舒适的最小高度
static void enlargeTable(QTableWidget* table, int rows = 4) {
    if (!table) return;
    constexpr int kRowHeight = 30;
    table->verticalHeader()->setDefaultSectionSize(kRowHeight);
    table->setMinimumHeight(46 + rows * kRowHeight);
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

// 每个页面顶部的标题 + 一句说明（subtitle 可省略：省略时不占位）
static void addPageHeader(QVBoxLayout* v, const QString& title, const QString& subtitle = {}) {
    auto* t = new QLabel(title);
    t->setObjectName("pageTitle");
    v->addWidget(t);
    if (!subtitle.isEmpty()) {
        auto* s = new QLabel(subtitle);
        s->setObjectName("pageSubtitle");
        s->setWordWrap(true);
        v->addWidget(s);
    }
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
        // 启动器自己崩掉时把调用栈写进 note/，下次启动能看到
        {
            const auto noteDir = layout_.gameDir / "note";
            std::error_code nec;
            std::filesystem::create_directories(noteDir, nec);
            const auto cp = noteDir / ("launcher-crash-" + core::trashStamp() + ".log");
            std::snprintf(g_crashLogPath, sizeof g_crashLogPath, "%s", cp.c_str());
        }
        defaultPalette_ = qApp->palette();
        requiredJava_ = core::requiredJavaMajor(layout_, cfg_.version);
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
        // 已装加载器：启动后自动检查一次最新稳定版，避免一直用旧加载器
        QTimer::singleShot(1200, this, [this] {
            if (!cfg_.profile.empty() || cfg_.fabric) refreshLoaderVersions();
        });
        // 上次启动器自己崩过就提一句，别让人以为没事发生
        {
            std::error_code cec;
            std::filesystem::path newest;
            const auto noteDir = layout_.gameDir / "note";
            for (std::filesystem::directory_iterator it(noteDir, cec), end; !cec && it != end;
                 it.increment(cec)) {
                if (it->path().filename().string().rfind("launcher-crash-", 0) != 0) continue;
                if (newest.empty() ||
                    it->last_write_time() > std::filesystem::last_write_time(newest))
                    newest = it->path();
            }
            if (!newest.empty())
                launchLog_->appendPlainText("上次启动器崩溃的调用栈：" +
                                            QString::fromStdString(newest.string()));
        }
        // 冒烟/文档用：MC263_SHOT=/tmp/x.png 时把首屏抓成 PNG 后退出
        const QByteArray shotPath = qgetenv("MC263_SHOT");
        if (!shotPath.isEmpty())
            QTimer::singleShot(1500, this, [this, shotPath] {
                grab().save(QString::fromLocal8Bit(shotPath));
                qApp->quit();
            });
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
    // 下载页只展示常用/主流版本，避免几十个补丁版本把下拉框塞满。
    // 想调整白名单只改这里；26.x 当前线全部保留，预览版不过滤。
    static bool keepDownloadVersion(const QString& id) {
        if (id.startsWith("26.")) return true;
        if (id.contains("snapshot", Qt::CaseInsensitive) ||
            id.contains("pre", Qt::CaseInsensitive) ||
            id.contains("rc", Qt::CaseInsensitive))
            return true;
        static const QStringList curated = {
            "1.12.2", "1.15.2", "1.16.5", "1.19.1", "1.19.2",
            "1.20.1", "1.20.6", "1.21", "1.21.1"};
        return curated.contains(id);
    }

    void applyOnlineVersions(const std::vector<std::string>& versions, const QString& err) {
        if (!err.isEmpty()) { downloadLog_->appendPlainText("在线版本获取失败: " + err); return; }
        QString cur = downloadVersion_->currentText();
        downloadVersion_->clear();
        int shown = 0;
        for (const auto& v : versions) {
            const QString id = QString::fromStdString(v);
            if (!keepDownloadVersion(id)) continue;
            downloadVersion_->addItem(id);
            ++shown;
        }
        if (!cur.isEmpty()) downloadVersion_->setCurrentText(cur);
        downloadLog_->appendPlainText("在线版本列表已更新（显示 " + QString::number(shown) +
                                      " / " + QString::number(versions.size()) + " 个）");
    }

protected:
    // 系统配色变了（KDE 切明暗、换配色方案）就重算一遍；Noctalia 那条路由文件监视负责。
    // 自己 setPalette 也会发 PaletteChange，所以用 lastPalette_ 比对，避免自激。
    void changeEvent(QEvent* ev) override {
        if (ev->type() == QEvent::PaletteChange || ev->type() == QEvent::ApplicationPaletteChange) {
            if (qApp && qApp->palette() != lastPalette_) {
                lastPalette_ = qApp->palette();
                applyPalette();
                applyStyleSheet();
            }
        }
        QMainWindow::changeEvent(ev);
    }

    void closeEvent(QCloseEvent* ev) override {
        collectUiToConfig();
        std::string err;
        cfg_.save(layout_.configFile, err);
        if (downloadCancel_) downloadCancel_->store(true);
        if (launchProc_ && launchProc_->state() != QProcess::NotRunning) launchProc_->terminate();
        QMainWindow::closeEvent(ev);
    }

private:
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
        nav_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        for (const char* s : {"启动游戏", "下载游戏", "资源下载", "版本管理", "游戏目录", "资源", "支持"})
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
        addPage(pageVersionManager());
        addPage(pageDirs());
        addPage(pageResources());
        addPage(pageSettings());
        v->addWidget(nav_, 0, Qt::AlignHCenter);
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
        addPageHeader(v, "启动游戏");
        auto* box = new QGroupBox("原版启动", w);
        auto* form = new QFormLayout(box);
        setCardTitle(box, form, "原版启动");
        launchVersion_ = new QComboBox(w); launchVersion_->setEditable(true);
        usernameEdit_ = new QLineEdit(w);
        form->addRow("游戏版本", launchVersion_);
        form->addRow("玩家名", usernameEdit_);
        v->addWidget(box);

        core::MemInfo mi = core::readMemInfo();
        memTotalMb_ = mi.totalMb >= 2048 ? static_cast<int>(mi.totalMb) : 4096;
        if (mi.totalMb == 0) mi.totalMb = static_cast<uint64_t>(memTotalMb_);
        memInfo_ = mi;
        maxMemoryMb_ = std::max(2048, memTotalMb_ * 3 / 4);
        auto* mbox = new QGroupBox("内存图形化配置", w);
        auto* mv = new QVBoxLayout(mbox);
        setCardTitle(mbox, mv, "内存图形化配置");
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
        memAuto_ = new QCheckBox("内存交给 JVM ", w);
        memAuto_->setChecked(cfg_.autoMemory);
        mv->addWidget(memAuto_);
        auto* memRow = new QHBoxLayout();
        memSlider_ = new QSlider(Qt::Horizontal, w);
        memSlider_->setRange(512, maxMemoryMb_); memSlider_->setSingleStep(256);
        memSpin_ = new QSpinBox(w); memSpin_->setRange(512, maxMemoryMb_); memSpin_->setSingleStep(256);
        memSpin_->setSuffix(" MB");
        memRow->addWidget(new QLabel("分给游戏", w));
        memRow->addWidget(memSlider_, 1); memRow->addWidget(memSpin_);
        mv->addLayout(memRow);
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
        auto* memTimer = new QTimer(this);
        connect(memTimer, &QTimer::timeout, this, &LauncherWindow::refreshMemory);
        memTimer->start(2000);
        refreshMemory();

        auto* row = new QHBoxLayout();
        launchBtn_ = new QPushButton("启动游戏", w);
        launchBtn_->setObjectName("primary");
        auto* refreshBtn = new QPushButton("刷新本地版本", w);
        auto* openDirBtn = new QPushButton("打开游戏目录", w);
        auto* noteBtn = new QPushButton("日志", w);
        noteBtn->setToolTip("日志写在 <游戏目录>/note/，最多保留 10 个");
        auto* clearLogBtn = new QPushButton("清空日志", w);
        clearLogBtn->setToolTip("只清空当前窗口输出，不删除 note/ 里的日志文件");
        row->addWidget(launchBtn_); row->addWidget(refreshBtn); row->addWidget(openDirBtn);
        row->addWidget(noteBtn); row->addWidget(clearLogBtn); row->addStretch(1);
        v->addLayout(row);
        launchState_ = new QLabel("就绪", w);
        v->addWidget(launchState_);
        launchLog_ = new QPlainTextEdit(w); launchLog_->setReadOnly(true);
        // 给启动输出足够空间，同时别把下面的内存卡片完全挤出视口
        launchLog_->setMinimumHeight(180);
        launchLog_->setMaximumHeight(240);
        v->addWidget(launchLog_, 1);
        v->addWidget(mbox);
        connect(launchBtn_, &QPushButton::clicked, this, &LauncherWindow::startLaunch);
        connect(refreshBtn, &QPushButton::clicked, this, &LauncherWindow::refreshLocalVersions);
        connect(clearLogBtn, &QPushButton::clicked, this, [this] {
            if (launchLog_) launchLog_->clear();
            if (launchState_) launchState_->setText("日志已清空");
        });
        connect(openDirBtn, &QPushButton::clicked, this, [this] {
            QProcess::startDetached("xdg-open", {QString::fromStdString(layout_.gameDir.string())});
        });
        connect(noteBtn, &QPushButton::clicked, this, [this] {
            const auto dir = layout_.gameDir / "note";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            QProcess::startDetached("xdg-open", {QString::fromStdString(dir.string())});
        });
        return w;
    }

    QWidget* pageDownload() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "下载游戏");
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
        snapCheck_ = new QCheckBox("含预览版", w);
        snapCheck_->setToolTip("勾上后「获取在线版本」会同时列出 snapshot（预览版）");
        auto* latestSnapBtn = new QPushButton("最新预览版", w);
        latestSnapBtn->setToolTip("把版本填成 Mojang 当前的 latest.snapshot，再点「开始下载」");
        auto* verRow = new QHBoxLayout();
        verRow->addWidget(downloadVersion_, 1);
        verRow->addWidget(onlineBtn);
        verRow->addWidget(snapCheck_);
        verRow->addWidget(latestSnapBtn);
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
        auto* verifyBtn = new QPushButton("校验已装文件", w);
        clearCacheBtn_->setToolTip("只删元数据缓存（版本清单 / Modrinth 查询 / 加载器 profile），已下载的游戏文件不动");
        verifyBtn->setToolTip("按 <版本>.json 的 SHA1 核对 jar / libraries / assets；缺的、坏的自动补下");
        row->addWidget(downloadBtn_);
        row->addWidget(cancelBtn_);
        row->addWidget(verifyBtn);
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
            if (downloadCancel_) {
                downloadCancel_->store(true);
                downloadLabel_->setText("正在取消...");
                cancelBtn_->setEnabled(false);
            }
        });
        connect(clearCacheBtn_, &QPushButton::clicked, this, &LauncherWindow::clearDownloadCache);
        connect(verifyBtn, &QPushButton::clicked, this, &LauncherWindow::verifyInstalled);
        connect(onlineBtn, &QPushButton::clicked, this, &LauncherWindow::refreshOnlineVersions);
        connect(latestSnapBtn, &QPushButton::clicked, this, &LauncherWindow::fetchLatestSnapshot);
        return w;
    }

    // ---- Mod 下载（Modrinth 公开 API）----
    QWidget* pageMods() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "资源下载",
                      "从 Modrinth 搜索 Mod / 资源包 / 光影，按游戏版本过滤，装进当前版本目录");
        const int firstCardIndex = v->count();

        auto* sbox = new QGroupBox("搜索", w);
        auto* form = new QFormLayout(sbox);
        setCardTitle(sbox, form, "搜索");
        modQuery_ = new QLineEdit(sbox);
        modQuery_->setPlaceholderText("sodium / lithium / jei …");
        modQuery_->setClearButtonEnabled(true);
        modCompleter_ = new QCompleter(this);
        modCompleter_->setCaseSensitivity(Qt::CaseInsensitive);
        modCompleter_->setFilterMode(Qt::MatchContains);
        modCompleter_->setCompletionMode(QCompleter::PopupCompletion);
        modCompleter_->setMaxVisibleItems(8);
        modCompleter_->setModel(new QStringListModel(this));
        modQuery_->setCompleter(modCompleter_);
        modSuggestTimer_ = new QTimer(this);
        modSuggestTimer_->setSingleShot(true);
        modSuggestTimer_->setInterval(300);
        connect(modQuery_, &QLineEdit::textEdited, this, [this](const QString& text) {
            if (modContentType_ && modContentType_->currentData().toString() != "mod") return;
            const int n = text.trimmed().size();
            if (n < 2) {
                if (modSuggestTimer_) modSuggestTimer_->stop();
                if (auto* model = qobject_cast<QStringListModel*>(modCompleter_->model()))
                    model->setStringList({});
                if (auto* popup = modCompleter_->popup()) popup->hide();
                return;
            }
            if (modSuggestTimer_) modSuggestTimer_->start();
        });
        connect(modSuggestTimer_, &QTimer::timeout, this, &LauncherWindow::requestModSuggestions);
        modContentType_ = new QComboBox(sbox);
        modContentType_->addItem("Mod", "mod");
        modContentType_->addItem("资源包", "resourcepack");
        modContentType_->addItem("光影", "shader");
        modVersionCombo_ = new QComboBox(sbox);
        modVersionCombo_->setToolTip(
            "列出所有已下载版本；显示实际版本目录（如 26.3+fabric），查询 Modrinth 时"
            "自动取基础游戏版本 26.3，并按目录后缀决定加载器。");
        refreshModVersionChoices();
        modSort_ = new QComboBox(sbox);
        modSort_->addItem("下载量", "downloads");
        modSort_->addItem("相关度", "relevance");
        modSort_->addItem("关注数", "follows");
        modSort_->addItem("最新发布", "newest");
        modSort_->addItem("最近更新", "updated");
        form->addRow("下载类型", modContentType_);
        form->addRow("关键词", modQuery_);
        form->addRow("游戏版本", modVersionCombo_);
        form->addRow("排序", modSort_);

        modSearchBtn_ = new QPushButton("搜索", sbox);
        modSearchBtn_->setObjectName("primary");
        auto* srow = new QHBoxLayout();
        srow->addWidget(modSearchBtn_);
        srow->addStretch(1);
        form->addRow(QString(), srow);
        v->addWidget(sbox);

        auto* lbox = new QGroupBox("加载器", w);
        auto* lform = new QFormLayout(lbox);
        setCardTitle(lbox, lform, "加载器（Fabric / Quilt）");
        loaderCombo_ = new QComboBox(lbox);
        loaderCombo_->addItem("Fabric", "fabric");
        loaderCombo_->addItem("Quilt", "quilt");
        if (cfg_.profile == "quilt") loaderCombo_->setCurrentIndex(1);
        loaderVerCombo_ = new QComboBox(lbox);
        loaderVerCombo_->setEnabled(false);
        loaderVerCombo_->setToolTip("加载器版本固定为最新稳定版");
        auto* getLv = new QPushButton("获取版本", lbox);
        auto* lrow = new QHBoxLayout();
        lrow->addWidget(loaderVerCombo_, 1);
        lrow->addWidget(getLv);
        installLoaderBtn_ = new QPushButton("安装 / 更新加载器", lbox);
        installLoaderBtn_->setObjectName("primary");
        loaderState_ = new QLabel("装完后启动会用 versions/<版本>+<加载器>，原版不受影响", lbox);
        loaderState_->setObjectName("muted");
        auto* loaderNote = new QLabel(
            "说明：安装加载器会自动生成独立版本目录，与原版隔开；启动页需手动把游戏版本切到加载器版本，否则仍按原版启动。"
            "加载器版本固定使用最新稳定版，进入本页会自动检查并更新已装加载器。",
            lbox);
        loaderNote->setObjectName("muted");
        loaderNote->setWordWrap(true);
        lform->addRow("加载器", loaderCombo_);
        lform->addRow("版本", lrow);
        auto* irow = new QHBoxLayout();
        irow->addWidget(installLoaderBtn_);
        irow->addStretch(1);
        lform->addRow(QString(), irow);
        lform->addRow(QString(), loaderState_);
        lform->addRow(QString(), loaderNote);
        modLoaderBox_ = lbox;
        v->insertWidget(firstCardIndex, lbox);
        updateLoaderCardVisibility();
        connect(modContentType_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { applyModContentType(); });
        connect(modVersionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { updateLoaderCardVisibility(); });


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
        modOpenDirBtn_ = openMods;
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
        applyModContentType();

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
            const auto dir = currentModsDir();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            QProcess::startDetached("xdg-open", {QString::fromStdString(dir.string())});
        });
        return w;
    }

    // 下拉框里是实际版本目录名（26.3+fabric）；给 Modrinth 的 gameVersion 必须用基础版本 26.3
    QString modSelectedVersion() const {
        return modVersionCombo_ ? modVersionCombo_->currentData().toString() : QString();
    }

    QString modGameVersion() const {
        const QString selected = modSelectedVersion();
        const int plus = selected.indexOf('+');
        return plus >= 0 ? selected.left(plus) : selected;
    }

    QString modContentType() const {
        return modContentType_ ? modContentType_->currentData().toString() : QStringLiteral("mod");
    }

    // 当前选中的版本只要已经装了 Fabric/Quilt，加载器卡片就没有意义了，整块隐藏。
    void updateLoaderCardVisibility() {
        if (!modLoaderBox_) return;
        if (modContentType() != "mod") { modLoaderBox_->hide(); return; }
        const QString base = modGameVersion();
        bool hasLoader = false;
        if (!base.isEmpty()) {
            for (const auto& id : core::availableVersions(layout_)) {
                const QString v = QString::fromStdString(id);
                if (v.startsWith(base + "+")) { hasLoader = true; break; }
            }
        }
        modLoaderBox_->setVisible(!hasLoader);
    }

    void applyModContentType() {
        const QString type = modContentType();
        updateLoaderCardVisibility();
        if (modOpenDirBtn_) modOpenDirBtn_->setVisible(type == "mod");
        if (modInstallBtn_)
            modInstallBtn_->setText(type == "mod" ? "安装选中 Mod" : "下载选中");
        if (modTable_) {
            modTable_->setRowCount(0);
            if (type == "mod")
                modTable_->setHorizontalHeaderLabels({"Mod", "作者", "下载量", "说明"});
            else if (type == "shader")
                modTable_->setHorizontalHeaderLabels({"光影", "作者", "下载量", "说明"});
            else
                modTable_->setHorizontalHeaderLabels({"资源包", "作者", "下载量", "说明"});
        }
        if (modQuery_) {
            if (type == "mod")
                modQuery_->setPlaceholderText("sodium / lithium / jei …");
            else if (type == "shader")
                modQuery_->setPlaceholderText("Complementary / BSL / SEUS …");
            else
                modQuery_->setPlaceholderText("Stay True / Faithless / 原版风格 …");
        }
        if (type != "mod" && modCompleter_) {
            if (auto* model = qobject_cast<QStringListModel*>(modCompleter_->model()))
                model->setStringList({});
            if (auto* popup = modCompleter_->popup()) popup->hide();
        }
        if (modState_ && modTable_ && modTable_->rowCount() == 0)
            modState_->setText(type == "mod" ? "输入关键词后点搜索"
                                             : "选择资源包 / 光影，输入关键词后点搜索");
    }

    // 加载器直接从版本目录名推导：26.3+fabric → fabric，26.3 → 原版（不限）
    QString modSelectedLoader() const {
        const QString selected = modSelectedVersion();
        const int plus = selected.lastIndexOf('+');
        return plus >= 0 ? selected.mid(plus + 1) : QString();
    }

    void refreshModVersionChoices() {
        if (!modVersionCombo_) return;
        const QString keep = modVersionCombo_->currentData().toString();
        const QString wanted =
            keep.isEmpty() ? QString::fromStdString(core::versionDirName(cfg_)) : keep;
        modVersionCombo_->clear();
        const auto vers = core::availableVersions(layout_);
        for (const auto& id : vers)
            modVersionCombo_->addItem(QString::fromStdString(id), QString::fromStdString(id));
        if (vers.empty()) {
            modVersionCombo_->addItem("未安装任何版本", "");
            modVersionCombo_->setEnabled(false);
            return;
        }
        modVersionCombo_->setEnabled(true);
        int idx = modVersionCombo_->findData(wanted);
        if (idx < 0) idx = 0;
        modVersionCombo_->setCurrentIndex(idx);
        updateLoaderCardVisibility();
    }

    void requestModSuggestions() {
        if (!modQuery_ || !modCompleter_) return;
        if (modContentType() != "mod") return;
        const QString q = modQuery_->text().trimmed();
        if (q.size() < 2) return;
        const QString loader = modSelectedLoader();
        const QString ver = modGameVersion();
        if (ver.isEmpty()) return;
        const QString key = q + QChar(0x1f) + loader + QChar(0x1f) + ver;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const auto cached = modSuggestCache_.constFind(key);
        const auto cachedAt = modSuggestCacheAt_.constFind(key);
        if (cached != modSuggestCache_.constEnd() && cachedAt != modSuggestCacheAt_.constEnd() &&
            now - cachedAt.value() < 5 * 60 * 1000) {
            applyModSuggestions(q, cached.value());
            return;
        }
        const auto cache = layout_.configDir / "cache";
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, q, key, loader, ver, cache] {
            std::vector<mc263::modrinth::Hit> hits;
            std::string err;
            const bool ok = mc263::modrinth::search(q.toStdString(), loader.toStdString(),
                                                    ver.toStdString(), "downloads", cache, hits, err);
            QStringList items;
            if (ok) {
                for (const auto& h : hits) {
                    const QString title = QString::fromStdString(h.title).trimmed();
                    if (!title.isEmpty() && !items.contains(title)) items << title;
                    if (items.size() >= 8) break;
                }
            }
            QMetaObject::invokeMethod(qApp, [guard, q, key, items, ok] {
                if (!guard || !guard->modCompleter_ || !guard->modQuery_) return;
                if (guard->modQuery_->text().trimmed() != q) return;
                if (ok) {
                    guard->modSuggestCache_.insert(key, items);
                    guard->modSuggestCacheAt_.insert(key, QDateTime::currentMSecsSinceEpoch());
                }
                guard->applyModSuggestions(q, items);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void applyModSuggestions(const QString& prefix, const QStringList& items) {
        if (!modCompleter_) return;
        auto* model = qobject_cast<QStringListModel*>(modCompleter_->model());
        if (!model) return;
        model->setStringList(items);
        if (items.isEmpty()) {
            if (auto* popup = modCompleter_->popup()) popup->hide();
            return;
        }
        modCompleter_->setCompletionPrefix(prefix);
        modCompleter_->complete();
    }

    void startModSearch() {
        const QString q = modQuery_->text().trimmed();
        if (q.isEmpty()) { modState_->setText("先输入关键词"); return; }
        const std::string ver = modGameVersion().toStdString();
        if (ver.empty()) { modState_->setText("先在下载页安装一个游戏版本"); return; }
        modSearchBtn_->setEnabled(false);
        modState_->setText("搜索中…");
        const QString type = modContentType();
        const std::string loader =
            type == "mod" ? modSelectedLoader().toStdString() : std::string();
        const std::string sort = modSort_->currentData().toString().toStdString();
        const auto cache = layout_.configDir / "cache";
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, q, loader, ver, sort, cache, type] {
            std::vector<mc263::modrinth::Hit> hits;
            std::string err;
            const bool ok = mc263::modrinth::search(q.toStdString(), loader, ver, sort, cache, hits,
                                                    err, type.toStdString());
            QMetaObject::invokeMethod(qApp, [guard, ok, err, hits, type] {
                if (guard) guard->applyModHits(ok, err, hits, type);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void applyModHits(bool ok, const std::string& err,
                      const std::vector<mc263::modrinth::Hit>& hits, const QString& type) {
        modSearchBtn_->setEnabled(true);
        if (type != modContentType()) return;
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
            name->setData(Qt::UserRole + 1, type);
            name->setToolTip(QString::fromStdString(h.id));
            modTable_->setItem(r, 0, name);
            modTable_->setItem(r, 1, new QTableWidgetItem(QString::fromStdString(h.author)));
            auto* dl = new QTableWidgetItem(QLocale().toString(static_cast<qulonglong>(h.downloads)));
            dl->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            modTable_->setItem(r, 2, dl);
            modTable_->setItem(r, 3, new QTableWidgetItem(QString::fromStdString(h.description)));
        }
        modState_->setText(hits.empty() ? "没有结果，换个关键词或放宽版本 / 加载器"
                                        : QString("找到 %1 个，双击或点「%2」")
                                              .arg(static_cast<int>(hits.size()))
                                              .arg(type == "mod" ? "安装选中 Mod" : "下载选中"));
    }

    // ---- 安装 Mod：先解析 required 前置，确认后一起下载 ----
    // 解析（网络）在后台线程；弹框在主线程；下载再开一个后台线程，避免界面卡住。
    void installSelectedMod() {
        const int row = modTable_->currentRow();
        if (row < 0) { modState_->setText("先在列表里选一个 Mod"); return; }
        auto* nameItem = modTable_->item(row, 0);
        const std::string project = nameItem->data(Qt::UserRole).toString().toStdString();
        const QString title = nameItem->text();
        QString type = nameItem->data(Qt::UserRole + 1).toString();
        if (type.isEmpty()) type = modContentType();
        if (type != "mod") {
            downloadSelectedContent(project, title, type);
            return;
        }
        const std::string loader = modSelectedLoader().toStdString();
        const std::string ver = modGameVersion().toStdString();
        if (ver.empty()) { modState_->setText("先在下载页安装一个游戏版本"); return; }
        modInstallBtn_->setEnabled(false);
        modState_->setText("获取 " + title + " 的版本列表…");
        const auto cache = layout_.configDir / "cache";
        const auto mods = currentModsDir();  // 版本隔离：mods 跟着下拉框选的版本走
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, project, loader, ver, cache, mods, title] {
            std::vector<mc263::modrinth::Version> list;
            std::string err;
            if (!mc263::modrinth::versions(project, loader, ver, cache, list, err)) {
                const QString msg = "查版本失败：" + QString::fromStdString(err);
                QMetaObject::invokeMethod(qApp, [guard, msg, title] {
                    if (!guard) return;
                    guard->modInstallBtn_->setEnabled(true);
                    guard->modState_->setText("失败");
                    guard->modLog_->appendPlainText(QString("[%1] %2  %3")
                        .arg(QTime::currentTime().toString("HH:mm:ss"), title, msg));
                }, Qt::QueuedConnection);
                return;
            }
            if (list.empty()) {
                QMetaObject::invokeMethod(qApp, [guard, title] {
                    if (!guard) return;
                    guard->modInstallBtn_->setEnabled(true);
                    guard->modState_->setText("没有适配该版本 / 加载器的文件");
                }, Qt::QueuedConnection);
                return;
            }
            std::vector<mc263::modrinth::Version> usable;
            for (const auto& v : list)
                if (mc263::modrinth::primaryFile(v)) usable.push_back(v);
            if (usable.empty()) {
                QMetaObject::invokeMethod(qApp, [guard, title, project] {
                    if (!guard) return;
                    guard->modInstallBtn_->setEnabled(true);
                    guard->promptManualMod(title,
                        QString::fromStdString("https://modrinth.com/mod/" + project));
                }, Qt::QueuedConnection);
                return;
            }
            QMetaObject::invokeMethod(qApp, [guard, usable, title, loader, ver, cache, mods] {
                if (guard) guard->chooseModVersion(usable, title, loader, ver, cache, mods);
            }, Qt::QueuedConnection);
        }).detach();
    }

    struct DepChoice {
        std::string projectId;
        std::string preferredVersionId;
        QString title;
        std::vector<mc263::modrinth::Version> versions;
    };
    struct SelectedDep {
        std::string projectId;
        QString title;
        mc263::modrinth::Version version;
    };

    // 选择要安装的 Mod 版本：R=Release（绿）、B=Beta（橙）、A=Alpha（灰）
    void chooseModVersion(const std::vector<mc263::modrinth::Version>& versions,
                          const QString& title, const std::string& loader, const std::string& ver,
                          const std::filesystem::path& cache,
                          const std::filesystem::path& mods) {
        QDialog dlg(this);
        dlg.setWindowTitle("选择 Mod 版本");
        auto* v = new QVBoxLayout(&dlg);
        const QString loaderLabel = loader.empty() ? "不限" : QString::fromStdString(loader);
        auto* tip = new QLabel(QString("为「%1」选择安装版本（%2 / %3）")
                                   .arg(title, loaderLabel, QString::fromStdString(ver)), &dlg);
        tip->setWordWrap(true);
        v->addWidget(tip);

        bool needsSodium = false;
        for (const auto& x : versions)
            for (const auto& dep : x.dependencies)
                if (dep.type == "required" && dep.projectId == "AANobbMI") needsSodium = true;
        if (needsSodium) {
            auto* depHint = new QLabel(
                "该 Mod 依赖 Sodium。Modrinth 只声明依赖项目、不提供 Sodium 版本范围；"
                "默认选最新正式版最稳。如果已装 Sodium 是 Beta，附加 Mod 可能需要对应 Beta 版本。",
                &dlg);
            depHint->setObjectName("muted");
            depHint->setWordWrap(true);
            v->addWidget(depHint);
        }

        auto* table = new QTableWidget(static_cast<int>(versions.size()), 3, &dlg);
        table->setHorizontalHeaderLabels({"版本", "类型", "文件"});
        table->horizontalHeader()->setStretchLastSection(true);
        table->verticalHeader()->setVisible(false);
        table->setShowGrid(false);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setMinimumHeight(280);
        for (int i = 0; i < static_cast<int>(versions.size()); ++i) {
            const auto& x = versions[static_cast<size_t>(i)];
            const QString type = QString::fromStdString(x.versionType).toLower();
            auto* num = new QTableWidgetItem(
                QString::fromStdString(x.number.empty() ? x.name : x.number));
            if (!x.name.empty()) num->setToolTip(QString::fromStdString(x.name));
            table->setItem(i, 0, num);

            QString badge = "R";
            QString label = "Release";
            QColor bg("#16a34a");
            if (type == "beta") {
                badge = "B";
                label = "Beta";
                bg = QColor("#f59e0b");
            } else if (type == "alpha") {
                badge = "A";
                label = "Alpha";
                bg = QColor("#6b7280");
            } else if (!type.isEmpty() && type != "release") {
                badge = "?";
                label = type;
                bg = QColor("#6b7280");
            }
            auto* kind = new QLabel(badge, table);
            kind->setAlignment(Qt::AlignCenter);
            kind->setMargin(0);
            kind->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            kind->setStyleSheet(QString("background:%1;color:#ffffff;border-radius:4px;"
                                        "font-weight:600;").arg(bg.name()));
            kind->setToolTip(label);
            table->setCellWidget(i, 1, kind);

            const auto* f = mc263::modrinth::primaryFile(x);
            auto* file = new QTableWidgetItem(f ? QString::fromStdString(f->filename) : QString());
            if (f) file->setToolTip(QString::fromStdString(f->filename));
            table->setItem(i, 2, file);
        }
        table->resizeColumnToContents(0);
        table->setColumnWidth(1, 56);
        int defaultRow = 0;
        for (int i = 0; i < static_cast<int>(versions.size()); ++i) {
            const QString type =
                QString::fromStdString(versions[static_cast<size_t>(i)].versionType).toLower();
            if (type.isEmpty() || type == "release") { defaultRow = i; break; }
        }
        table->selectRow(defaultRow);
        v->addWidget(table, 1);
        dlg.resize(760, 460);

        auto* legend = new QLabel(
            "R = Release 稳定版（绿）    B = Beta 测试版（橙）    A = Alpha 早期版（灰）", &dlg);
        legend->setObjectName("muted");
        v->addWidget(legend);

        auto* row = new QHBoxLayout();
        row->addStretch(1);
        auto* go = new QPushButton("安装此版本", &dlg);
        go->setObjectName("primary");
        auto* cancel = new QPushButton("取消", &dlg);
        row->addWidget(go);
        row->addWidget(cancel);
        v->addLayout(row);
        connect(go, &QPushButton::clicked, &dlg, &QDialog::accept);
        connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
        connect(table, &QTableWidget::cellDoubleClicked, &dlg,
                [&dlg](int, int) { dlg.accept(); });

        if (dlg.exec() != QDialog::Accepted) {
            modInstallBtn_->setEnabled(true);
            modState_->setText("已取消");
            return;
        }
        const int rowIdx = table->currentRow();
        if (rowIdx < 0 || rowIdx >= static_cast<int>(versions.size())) {
            modInstallBtn_->setEnabled(true);
            modState_->setText("已取消");
            return;
        }
        mc263::modrinth::Version root = versions[static_cast<size_t>(rowIdx)];
        modState_->setText("检查 " + title + " 的前置…");
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, root, loader, ver, cache, mods, title] {
            std::vector<DepChoice> choices;
            std::vector<mc263::modrinth::DepItem> manual;
            std::set<std::string> seen;
            const auto installed = core::listEntries(mods);
            QHash<QString, QString> fileProject;
            auto depInstalled = [&](const std::string& projectId, const std::string& slug) {
                if (slug.empty()) return false;
                const QString slugQ = QString::fromStdString(slug);
                for (const auto& e : installed) {
                    if (e.isDir || !e.enabled) continue;
                    const QString name = QString::fromStdString(e.name);
                    if (!name.endsWith(".jar", Qt::CaseInsensitive)) continue;
                    if (!name.contains(slugQ, Qt::CaseInsensitive)) continue;
                    const QString key = QString::fromStdString(e.path.string());
                    if (!fileProject.contains(key)) {
                        std::string ierr;
                        const std::string sha = mc263::dl::Sha1::ofFile(e.path, ierr);
                        QString pid;
                        if (ierr.empty()) {
                            mc263::modrinth::Version iv;
                            std::string verr;
                            if (mc263::modrinth::versionByHash(sha, cache, iv, verr))
                                pid = QString::fromStdString(iv.projectId);
                        }
                        fileProject.insert(key, pid);
                    }
                    if (fileProject.value(key) == QString::fromStdString(projectId)) return true;
                }
                return false;
            };
            for (const auto& d : root.dependencies) {
                if (d.type != "required") continue;
                if (d.projectId.empty()) {
                    mc263::modrinth::DepItem item;
                    item.title = d.fileName.empty() ? "未知前置" : d.fileName;
                    item.reason = "Modrinth 上没有这个前置（作者只留了文件名）";
                    item.url = "https://modrinth.com/mods?q=" +
                               mc263::modrinth::urlEncode(d.fileName);
                    manual.push_back(std::move(item));
                    continue;
                }
                if (!seen.insert(d.projectId).second) continue;
                mc263::modrinth::Project p;
                std::string perr;
                const bool gotProject = mc263::modrinth::project(d.projectId, cache, p, perr);
                DepChoice ch;
                ch.projectId = d.projectId;
                ch.preferredVersionId = d.versionId;
                ch.title = QString::fromStdString(
                    gotProject && !p.title.empty() ? p.title : d.projectId);
                // 已安装在当前版本 mods/ 里的前置不再要求选择/重复下载
                if (depInstalled(d.projectId, gotProject ? p.slug : std::string())) continue;
                std::vector<mc263::modrinth::Version> list;
                std::string lerr;
                if (!mc263::modrinth::versions(d.projectId, loader, ver, cache, list, lerr)) {
                    mc263::modrinth::DepItem item;
                    item.projectId = d.projectId;
                    item.title = ch.title.toStdString();
                    item.reason = "查询前置版本失败：" + lerr;
                    item.url = "https://modrinth.com/mod/" + d.projectId;
                    manual.push_back(std::move(item));
                    continue;
                }
                for (const auto& v : list)
                    if (mc263::modrinth::primaryFile(v)) ch.versions.push_back(v);
                if (ch.versions.empty()) {
                    mc263::modrinth::DepItem item;
                    item.projectId = d.projectId;
                    item.title = ch.title.toStdString();
                    item.reason = "没有适配该版本 / 加载器的文件";
                    item.url = "https://modrinth.com/mod/" + d.projectId;
                    manual.push_back(std::move(item));
                    continue;
                }
                choices.push_back(std::move(ch));
            }
            QMetaObject::invokeMethod(qApp, [guard, root, title, loader, ver, cache, mods,
                                            choices, manual] {
                if (guard)
                    guard->showDependencyChoices(root, title, loader, ver, cache, mods,
                                                 choices, manual);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void showDependencyChoices(const mc263::modrinth::Version& root, const QString& title,
                               const std::string& loader, const std::string& ver,
                               const std::filesystem::path& cache,
                               const std::filesystem::path& mods,
                               const std::vector<DepChoice>& choices,
                               const std::vector<mc263::modrinth::DepItem>& manual) {
        if (choices.empty()) {
            mc263::modrinth::DepPlan plan;
            plan.manual = manual;
            askInstallPlan(title, root, plan, mods);
            return;
        }

        QDialog dlg(this);
        dlg.setWindowTitle("选择前置 Mod 版本");
        auto* v = new QVBoxLayout(&dlg);
        auto* tip = new QLabel(
            QString("安装「%1」会一起安装以下前置。请选择每个前置要用的版本：").arg(title), &dlg);
        tip->setWordWrap(true);
        v->addWidget(tip);

        auto* table = new QTableWidget(static_cast<int>(choices.size()), 3, &dlg);
        table->setHorizontalHeaderLabels({"前置 Mod", "版本", "文件"});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->horizontalHeader()->setStretchLastSection(true);
        table->verticalHeader()->setVisible(false);
        table->setShowGrid(false);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setMinimumHeight(48 + static_cast<int>(choices.size()) * 36);
        std::vector<QComboBox*> combos;
        for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
            const auto& ch = choices[static_cast<size_t>(i)];
            table->setItem(i, 0, new QTableWidgetItem(ch.title));
            auto* combo = new QComboBox(table);
            auto* model = new QStandardItemModel(combo);
            QStringList files;
            int defaultIdx = -1;
            int releaseIdx = -1;
            for (int j = 0; j < static_cast<int>(ch.versions.size()); ++j) {
                const auto& x = ch.versions[static_cast<size_t>(j)];
                const QString type = QString::fromStdString(x.versionType).toLower();
                QString badge = "R";
                QColor color("#16a34a");
                if (type == "beta") {
                    badge = "B";
                    color = QColor("#f59e0b");
                } else if (type == "alpha") {
                    badge = "A";
                    color = QColor("#6b7280");
                } else if (!type.isEmpty() && type != "release") {
                    badge = "?";
                    color = QColor("#6b7280");
                }
                const QString number = QString::fromStdString(
                    x.number.empty() ? (x.name.empty() ? x.id : x.name) : x.number);
                auto* item = new QStandardItem(badge + "  " + number);
                item->setForeground(color);
                item->setData(j, Qt::UserRole);
                model->appendRow(item);
                const auto* f = mc263::modrinth::primaryFile(x);
                files << (f ? QString::fromStdString(f->filename) : QString());
                if ((type.isEmpty() || type == "release") && releaseIdx < 0) releaseIdx = j;
                if (!ch.preferredVersionId.empty() && x.id == ch.preferredVersionId) defaultIdx = j;
            }
            combo->setModel(model);
            combo->setCurrentIndex(defaultIdx >= 0 ? defaultIdx
                                                   : (releaseIdx >= 0 ? releaseIdx : 0));
            table->setCellWidget(i, 1, combo);
            const int sel = combo->currentIndex();
            auto* file = new QTableWidgetItem(files.value(sel));
            table->setItem(i, 2, file);
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg,
                    [file, files](int idx) {
                        if (idx >= 0 && idx < files.size()) file->setText(files[idx]);
                    });
            combos.push_back(combo);
        }
        table->setColumnWidth(1, 220);
        v->addWidget(table, 1);
        auto* legend = new QLabel(
            "R = Release 正式版（推荐）    B = Beta    A = Alpha", &dlg);
        legend->setObjectName("muted");
        v->addWidget(legend);
        if (!manual.empty()) {
            auto* miss = new QLabel(
                QString("另有 %1 个前置无法自动下载，下一步会给出网页链接。").arg(manual.size()),
                &dlg);
            miss->setObjectName("muted");
            miss->setWordWrap(true);
            v->addWidget(miss);
        }
        dlg.resize(760, 420);
        auto* row = new QHBoxLayout();
        row->addStretch(1);
        auto* go = new QPushButton("一起安装", &dlg);
        go->setObjectName("primary");
        auto* cancel = new QPushButton("取消", &dlg);
        row->addWidget(go);
        row->addWidget(cancel);
        v->addLayout(row);
        connect(go, &QPushButton::clicked, &dlg, &QDialog::accept);
        connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) {
            modInstallBtn_->setEnabled(true);
            modState_->setText("已取消");
            return;
        }

        std::vector<SelectedDep> selected;
        for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
            const int idx = combos[i]->currentData().toInt();
            if (idx < 0 || idx >= static_cast<int>(choices[static_cast<size_t>(i)].versions.size()))
                continue;
            selected.push_back(SelectedDep{
                choices[static_cast<size_t>(i)].projectId,
                choices[static_cast<size_t>(i)].title,
                choices[static_cast<size_t>(i)].versions[static_cast<size_t>(idx)]});
        }
        modState_->setText("解析前置依赖…");
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, root, title, selected, manual, loader, ver, cache, mods] {
            mc263::modrinth::DepPlan plan;
            std::set<std::string> seen;
            std::set<std::string> manualSeen;
            auto addManual = [&](const mc263::modrinth::DepItem& x) {
                const std::string key = x.projectId + "|" + x.fileName + "|" + x.url;
                if (manualSeen.insert(key).second) plan.manual.push_back(x);
            };
            for (const auto& sd : selected) {
                if (!seen.insert(sd.projectId).second) continue;
                mc263::modrinth::DepItem item;
                item.projectId = sd.projectId;
                item.versionId = sd.version.id;
                item.versionNumber = sd.version.number;
                item.title = sd.title.toStdString();
                const auto* f = mc263::modrinth::primaryFile(sd.version);
                if (!f) {
                    item.reason = "没有可下载的主文件";
                    addManual(item);
                    continue;
                }
                item.file = *f;
                plan.resolved.push_back(std::move(item));
            }
            std::string err;
            for (const auto& sd : selected) {
                mc263::modrinth::DepPlan sub;
                mc263::modrinth::resolveDependencies(sd.version, loader, ver, cache, sub, err);
                for (auto& x : sub.resolved)
                    if (seen.insert(x.projectId).second) plan.resolved.push_back(std::move(x));
                for (const auto& x : sub.manual) addManual(x);
            }
            for (const auto& x : manual) addManual(x);
            QMetaObject::invokeMethod(qApp, [guard, root, plan, mods, title] {
                if (guard) guard->askInstallPlan(title, root, plan, mods);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void downloadSelectedContent(const std::string& project, const QString& title,
                                 const QString& type) {
        const std::string ver = modGameVersion().toStdString();
        if (ver.empty()) { modState_->setText("先在下载页安装一个游戏版本"); return; }
        modInstallBtn_->setEnabled(false);
        modState_->setText("获取 " + title + " 的版本…");
        const auto cache = layout_.configDir / "cache";
        const QString sel = modSelectedVersion();
        const auto base = sel.isEmpty()
                              ? core::isolatedGameDir(layout_, cfg_)
                              : layout_.versionsDir / sel.toStdString();
        const auto dest = base / (type == "shader" ? "shaderpacks" : "resourcepacks");
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, project, ver, cache, dest, title] {
            std::vector<mc263::modrinth::Version> list;
            std::string err;
            if (!mc263::modrinth::versions(project, "", ver, cache, list, err)) {
                QMetaObject::invokeMethod(qApp, [guard, err] {
                    if (!guard) return;
                    guard->modInstallBtn_->setEnabled(true);
                    guard->modState_->setText("获取版本失败：" + QString::fromStdString(err));
                }, Qt::QueuedConnection);
                return;
            }
            const mc263::modrinth::Version* pick = nullptr;
            for (const auto& x : list)
                if (mc263::modrinth::primaryFile(x) &&
                    (x.versionType.empty() || x.versionType == "release")) { pick = &x; break; }
            if (!pick)
                for (const auto& x : list)
                    if (mc263::modrinth::primaryFile(x)) { pick = &x; break; }
            if (!pick) {
                QMetaObject::invokeMethod(qApp, [guard] {
                    if (!guard) return;
                    guard->modInstallBtn_->setEnabled(true);
                    guard->modState_->setText("没有适配该游戏版本的文件");
                }, Qt::QueuedConnection);
                return;
            }
            const auto* f = mc263::modrinth::primaryFile(*pick);
            std::error_code ec;
            std::filesystem::create_directories(dest, ec);
            mc263::dl::Task task{f->url, dest / f->filename, f->sha1, 0644, f->filename};
            std::string derr;
            bool cached = false;
            const bool ok = mc263::dl::fetchOne(task, "", 180, true, derr, cached);
            const QString fileName = QString::fromStdString(f->filename);
            QMetaObject::invokeMethod(qApp, [guard, ok, derr, fileName, cached, title] {
                if (!guard) return;
                guard->modInstallBtn_->setEnabled(true);
                if (!ok) {
                    guard->modState_->setText("下载失败：" + QString::fromStdString(derr));
                    return;
                }
                guard->modState_->setText(
                    QString("%1 %2：%3").arg(title, cached ? "已存在" : "已下载", fileName));
                guard->modLog_->appendPlainText(
                    QString("[%1] %2 %3").arg(QTime::currentTime().toString("HH:mm:ss"),
                                              title, cached ? "已存在" : "下载完成"));
                guard->refreshResources();
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
                guard->refreshInstalledMods();
            }, Qt::QueuedConnection);
        }).detach();
    }

    // 版本管理：左栏列已装版本，右栏是 Java 与兼容性（原「Java 与兼容性」页融进来）
    // ---- 已装 Mod 管理 ----
    std::filesystem::path currentModsDir() const {
        const QString v = modSelectedVersion();
        if (!v.isEmpty()) return layout_.versionsDir / v.toStdString() / "mods";
        return core::isolatedGameDir(layout_, cfg_) / "mods";
    }

    void refreshInstalledMods() {
        if (!instModTable_) return;
        const auto dir = currentModsDir();
        // 只读：不要 create_directories，否则点开页面就会凭空生成版本目录
        const auto entries = core::listEntries(dir);
        instModTable_->setRowCount(0);
        int enabledCount = 0;
        for (const auto& e : entries) {
            if (e.isDir) continue;
            const int r = instModTable_->rowCount();
            instModTable_->insertRow(r);
            auto* nameItem = new QTableWidgetItem(QString::fromStdString(e.name));
            nameItem->setData(Qt::UserRole, QString::fromStdString(e.path.string()));
            nameItem->setToolTip(QString::fromStdString(e.name));
            instModTable_->setItem(r, 0, nameItem);
            instModTable_->setItem(r, 1, new QTableWidgetItem(e.enabled ? "启用" : "已禁用"));
            if (e.enabled) ++enabledCount;
        }
        instModTable_->resizeRowsToContents();
        instModTable_->scrollToTop();
        if (instModState_)
            instModState_->setText(QString("共 %1 个，启用 %2 个；目录：%3")
                                       .arg(instModTable_->rowCount())
                                       .arg(enabledCount)
                                       .arg(QString::fromStdString(dir.string())));
    }

    void toggleInstalledMod() {
        if (!instModTable_ || !instModTable_->selectionModel()) return;
        const auto rows = instModTable_->selectionModel()->selectedRows();
        if (rows.isEmpty()) { instModState_->setText("先在列表里选一个"); return; }
        int changed = 0;
        for (const auto& idx : rows) {
            auto* nameItem = instModTable_->item(idx.row(), 0);
            auto* stateItem = instModTable_->item(idx.row(), 1);
            if (!nameItem) continue;
            const bool enabled = stateItem && stateItem->text() == "启用";
            std::string err;
            if (core::setEntryEnabled(std::filesystem::path(nameItem->data(Qt::UserRole).toString().toStdString()),
                                      !enabled, err))
                ++changed;
            else
                modLog_->appendPlainText("切换失败：" + QString::fromStdString(err));
        }
        refreshInstalledMods();
        instModState_->setText(QString("已切换 %1 个（.disabled 后缀 = 游戏不加载）").arg(changed));
    }

    void deleteInstalledMod() {
        if (!instModTable_ || !instModTable_->selectionModel()) return;
        const auto rows = instModTable_->selectionModel()->selectedRows();
        if (rows.isEmpty()) { instModState_->setText("先在列表里选一个"); return; }
        if (QMessageBox::question(this, "删除 Mod",
                                  QString("删除选中的 %1 个 Mod？会移到回收站（资源页「回收站」标签可恢复）")
                                      .arg(rows.size()),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        int removed = 0;
        for (const auto& idx : rows) {
            auto* nameItem = instModTable_->item(idx.row(), 0);
            if (!nameItem) continue;
            std::string err;
            std::filesystem::path moved;
            if (core::moveToTrash(std::filesystem::path(nameItem->data(Qt::UserRole).toString().toStdString()),
                                  core::trashRoot(layout_), moved, err))
                ++removed;
            else
                modLog_->appendPlainText("删除失败：" + QString::fromStdString(err));
        }
        refreshInstalledMods();
        instModState_->setText(QString("已移到回收站 %1 个（资源页「回收站」可恢复）").arg(removed));
    }

    // 更新检查：本地 jar 的 SHA1 → Modrinth /version_file → 项目最新兼容版本
    void updateInstalledMods() {
        if (!instModTable_) return;
        const auto dir = currentModsDir();
        auto entries = core::listEntries(dir);
        if (entries.empty()) { instModState_->setText("没有已装的 Mod"); return; }
        const std::string loader = modSelectedLoader().toStdString();
        const std::string ver = modGameVersion().toStdString();
        if (ver.empty()) { instModState_->setText("先在下载页安装一个游戏版本"); return; }
        const auto cache = layout_.configDir / "cache";
        instModState_->setText("检查更新中…");
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, entries, dir, loader, ver, cache] {
            QStringList lines;
            int updated = 0, skipped = 0, failed = 0;
            for (const auto& e : entries) {
                if (e.isDir) continue;
                const QString name = QString::fromStdString(e.name);
                std::string err;
                const std::string sha = mc263::dl::Sha1::ofFile(e.path, err);
                if (!err.empty()) { ++failed; lines << name + "  读文件失败"; continue; }
                mc263::modrinth::Version cur;
                if (!mc263::modrinth::versionByHash(sha, cache, cur, err)) {
                    ++skipped; lines << name + "  不在 Modrinth 上，跳过"; continue;
                }
                std::vector<mc263::modrinth::Version> list;
                if (!mc263::modrinth::versions(cur.projectId, loader, ver, cache, list, err) || list.empty()) {
                    ++skipped; lines << name + "  没有适配 " + QString::fromStdString(ver) + " 的版本，跳过"; continue;
                }
                if (list.front().id == cur.id) { ++skipped; lines << name + "  已是最新"; continue; }
                const auto* f = mc263::modrinth::primaryFile(list.front());
                if (!f) { ++skipped; lines << name + "  新版本没有主文件"; continue; }
                mc263::dl::Task t{f->url, dir / f->filename, f->sha1, 0644, f->filename};
                bool cached = false;
                if (mc263::dl::fetchOne(t, "", 180, true, err, cached)) {
                    std::error_code rec;
                    std::filesystem::remove(e.path, rec);
                    ++updated;
                    lines << name + "  → " + QString::fromStdString(f->filename);
                } else {
                    ++failed;
                    lines << name + "  下载失败：" + QString::fromStdString(err);
                }
            }
            QMetaObject::invokeMethod(qApp, [guard, lines, updated, skipped, failed] {
                if (!guard) return;
                guard->refreshInstalledMods();
                guard->instModState_->setText(
                    QString("更新 %1 个，跳过 %2 个，失败 %3 个").arg(updated).arg(skipped).arg(failed));
                guard->modLog_->appendPlainText(lines.join("\n"));
            }, Qt::QueuedConnection);
        }).detach();
    }

    // 已装 Mod 管理卡片：放进版本管理页顶部，跟随左侧选中的版本
    QWidget* installedModsCard(QWidget* parent) {
        auto* ibox = new QGroupBox("已装 Mod（当前版本目录）", parent);
        auto* iv = new QVBoxLayout(ibox);
        setCardTitle(ibox, iv, "已装 Mod（当前版本目录）");
        instModTable_ = new QTableWidget(0, 2, ibox);
        instModTable_->setHorizontalHeaderLabels({"文件", "状态"});
        instModTable_->horizontalHeader()->setStretchLastSection(false);
        instModTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        instModTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        instModTable_->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        instModTable_->setWordWrap(true);
        instModTable_->setTextElideMode(Qt::ElideNone);
        instModTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        instModTable_->verticalHeader()->setVisible(false);
        instModTable_->setShowGrid(false);
        instModTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        instModTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        instModTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        enlargeTable(instModTable_, 7);
        iv->addWidget(instModTable_, 1);
        auto* mrow = new QHBoxLayout();
        auto* iRefresh = new QPushButton("刷新", ibox);
        auto* iToggle = new QPushButton("启用 / 禁用", ibox);
        auto* iDelete = new QPushButton("删除", ibox);
        auto* iUpdate = new QPushButton("检查更新", ibox);
        auto* iOpen = new QPushButton("打开目录", ibox);
        mrow->addWidget(iRefresh);
        mrow->addWidget(iToggle);
        mrow->addWidget(iDelete);
        mrow->addWidget(iUpdate);
        mrow->addWidget(iOpen);
        mrow->addStretch(1);
        iv->addLayout(mrow);
        instModState_ = new QLabel("切到这一页会自动刷新；更新只认 Modrinth 上有的文件", ibox);
        instModState_->setObjectName("muted");
        iv->addWidget(instModState_);

        connect(iRefresh, &QPushButton::clicked, this, &LauncherWindow::refreshInstalledMods);
        connect(iToggle, &QPushButton::clicked, this, &LauncherWindow::toggleInstalledMod);
        connect(iDelete, &QPushButton::clicked, this, &LauncherWindow::deleteInstalledMod);
        connect(iUpdate, &QPushButton::clicked, this, &LauncherWindow::updateInstalledMods);
        connect(iOpen, &QPushButton::clicked, this, [this] {
            const auto dir = currentModsDir();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            QProcess::startDetached("xdg-open", {QString::fromStdString(dir.string())});
        });
        return ibox;
    }

    QWidget* pageVersionManager() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "版本管理",
                      "左边是已装版本；右侧索引可切换「Mod 管理」「Java 设置」「JVM / 显示」");

        auto* row = new QHBoxLayout();
        row->setSpacing(12);

        auto* left = new QWidget(w);
        left->setFixedWidth(250);
        auto* lv = new QVBoxLayout(left);
        lv->setContentsMargins(0, 0, 0, 0);
        lv->setSpacing(6);
        auto* lhead = new QLabel("已装版本", left);
        lhead->setObjectName("cardTitle");
        lv->addWidget(lhead);
        verList_ = new QListWidget(left);
        verList_->setObjectName("verList");
        verList_->setFrameShape(QFrame::NoFrame);
        verList_->setSelectionMode(QAbstractItemView::SingleSelection);
        verList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        lv->addWidget(verList_, 1);
        auto* lrow = new QHBoxLayout();
        auto* refreshBtn = new QPushButton("刷新", left);
        auto* openDirBtn = new QPushButton("打开目录", left);
        lrow->addWidget(refreshBtn);
        lrow->addWidget(openDirBtn);
        lrow->addStretch(1);
        lv->addLayout(lrow);
        row->addWidget(left);

        auto* right = new QWidget(w);
        auto* rv = new QVBoxLayout(right);
        rv->setContentsMargins(0, 0, 0, 0);
        rv->setSpacing(10);
        auto* modsCard = installedModsCard(right);
        rv->addWidget(modsCard, 3);
        verSummary_ = new QLabel(right);
        verSummary_->setObjectName("pageSubtitle");
        verSummary_->setWordWrap(true);
        verSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* useVerBtn = new QPushButton("设为启动版本", right);
        useVerBtn->setObjectName("primary");
        auto* srow = new QHBoxLayout();
        srow->addWidget(verSummary_, 1);
        srow->addWidget(useVerBtn);
        rv->addLayout(srow);

        auto* jbox = new QGroupBox("可用的 Java", right);
        auto* jv = new QVBoxLayout(jbox);
        setCardTitle(jbox, jv, "可用的 Java");
        javaTable_ = new QTableWidget(0, 4, right);
        javaTable_->setHorizontalHeaderLabels({"路径", "版本", "来源", "状态"});
        javaTable_->horizontalHeader()->setStretchLastSection(true);
        javaTable_->verticalHeader()->setVisible(false);
        javaTable_->setShowGrid(false);
        javaTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        enlargeTable(javaTable_, 4);
        jv->addWidget(javaTable_);
        javaCombo_ = new QComboBox(right);
        javaState_ = new QLabel("尚未扫描", right);
        auto* jrow = new QHBoxLayout();
        auto* rescan = new QPushButton("重新扫描", right);
        auto* manual = new QPushButton("手动指定 java", right);
        auto* useBtn = new QPushButton("设为当前 Java", right);
        jrow->addWidget(rescan);
        jrow->addWidget(manual);
        jrow->addWidget(useBtn);
        jrow->addStretch(1);
        jv->addLayout(jrow);
        jv->addWidget(new QLabel("当前选择", right));
        jv->addWidget(javaCombo_);
        jv->addWidget(javaState_);
        rv->addWidget(jbox, 2);

        auto* vjbox = new QGroupBox("JVM / 显示（当前版本）", right);
        auto* vjv = new QFormLayout(vjbox);
        setCardTitle(vjbox, vjv, "JVM / 显示（当前版本）");
        extraArgsEdit_ = new QLineEdit(vjbox);
        extraArgsEdit_->setPlaceholderText("例如 滚木！");
        vjv->addRow("额外 JVM 参数", extraArgsEdit_);
        langCombo_ = new QComboBox(vjbox);
        langCombo_->addItem(QString("跟随系统(%1)")
                                .arg(QString::fromStdString(core::detectSystemLanguage())), QString());
        for (const char* code : {"en_us", "zh_cn", "zh_tw", "ja_jp", "ko_kr", "de_de",
                                  "fr_fr", "es_es", "pt_br", "ru_ru"})
            langCombo_->addItem(QString::fromLatin1(code), QString::fromLatin1(code));
        vjv->addRow("游戏语言", langCombo_);
        gameDisplay_ = new QComboBox(vjbox);
        gameDisplay_->addItem("自动（让游戏自己选）", "auto");
        gameDisplay_->addItem("Wayland 原生", "wayland");
        gameDisplay_->addItem("XWayland（强制 x11）", "x11");
        gameDisplay_->setToolTip(
            "只影响游戏进程的 SDL 后端。niri 没有 fifo-v1 时 SDL 会自动退回 XWayland；\n"
            "想强制原生 Wayland 选第二项（SDL_VIDEO_DRIVER=wayland）。");
        vjv->addRow("游戏显示后端", gameDisplay_);
        connect(extraArgsEdit_, &QLineEdit::textChanged, this, [this] { saveVersionJvmOptions(); });
        connect(langCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { saveVersionJvmOptions(); });
        connect(gameDisplay_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { saveVersionJvmOptions(); });
        rv->addWidget(vjbox, 2);

        auto* cbox = new QGroupBox("兼容性查验", right);
        auto* cv = new QVBoxLayout(cbox);
        setCardTitle(cbox, cv, "兼容性查验");
        compatTable_ = new QTableWidget(0, 3, right);
        compatTable_->setHorizontalHeaderLabels({"检查项", "状态", "说明"});
        compatTable_->horizontalHeader()->setStretchLastSection(true);
        compatTable_->verticalHeader()->setVisible(false);
        compatTable_->setShowGrid(false);
        compatTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        enlargeTable(compatTable_, 4);
        cv->addWidget(compatTable_);
        compatState_ = new QLabel("尚未检查", right);
        netCheck_ = new QCheckBox("同时检查官方下载源连通性", right);
        auto* checkBtn = new QPushButton("运行兼容性查验", right);
        auto* crow = new QHBoxLayout();
        crow->addWidget(checkBtn);
        crow->addWidget(netCheck_);
        crow->addStretch(1);
        cv->addLayout(crow);
        cv->addWidget(compatState_);
        rv->addWidget(cbox, 3);
        rv->addStretch(1);

        auto* scroll = new QScrollArea(w);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(right);
        row->addWidget(scroll, 1);

        auto* index = new QListWidget(w);
        index->setFixedWidth(120);
        index->setFrameShape(QFrame::NoFrame);
        index->addItem("Mod 管理");
        index->addItem("Java 设置");
        index->addItem("JVM / 显示");
        index->setCurrentRow(0);
        connect(index, &QListWidget::currentRowChanged, this,
                [scroll, modsCard, jbox, vjbox](int idx) {
                    QWidget* target = idx == 2 ? static_cast<QWidget*>(vjbox)
                                               : (idx == 1 ? static_cast<QWidget*>(jbox)
                                                           : modsCard);
                    scroll->ensureWidgetVisible(target);
                });
        row->addWidget(index);
        v->addLayout(row, 1);

        connect(verList_, &QListWidget::currentRowChanged, this, [this](int r) {
            if (r >= 0 && verList_->item(r)) showVersionDetail(verList_->item(r)->text());
        });
        connect(refreshBtn, &QPushButton::clicked, this, &LauncherWindow::refreshVersionList);
        connect(useVerBtn, &QPushButton::clicked, this, [this] {
            const auto* it = verList_->currentItem();
            if (!it) return;
            cfg_.version = it->text().toStdString();
            launchVersion_->setCurrentText(it->text());
            saveConfigQuietly();
            statusBar()->showMessage("启动版本已设为 " + it->text(), 5000);
        });
        connect(openDirBtn, &QPushButton::clicked, this, [this] {
            const auto* it = verList_->currentItem();
            if (!it) return;
            QProcess::startDetached(
                "xdg-open",
                {QString::fromStdString(layout_.versionDir(it->text().toStdString()).string())});
        });
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

        refreshVersionList();
        return w;
    }

    // 左栏列表：已装版本
    void refreshVersionList() {
        if (!verList_) return;
        const QString keep = verList_->currentItem() ? verList_->currentItem()->text() : QString();
        const auto vers = core::availableVersions(layout_);
        verList_->clear();
        for (const auto& id : vers) verList_->addItem(QString::fromStdString(id));
        if (verList_->count() == 0) {
            if (verSummary_) verSummary_->setText("还没有装任何游戏版本；去'下载页'拉一个，或先装个加载器");
            return;
        }
        int idx = 0;
        if (!keep.isEmpty()) {
            const auto hits = verList_->findItems(keep, Qt::MatchExactly);
            if (!hits.isEmpty()) idx = verList_->row(hits.first());
        }
        verList_->setCurrentRow(idx);
        if (verList_->item(idx)) showVersionDetail(verList_->item(idx)->text());
    }

    // 右栏头部：所选版本的要求与占用
    void showVersionDetail(const QString& id) {
        if (!verSummary_) return;
        const std::string vid = id.toStdString();
        const auto dir = layout_.versionDir(vid);
        const int java = core::requiredJavaMajor(layout_, vid);
        auto count = [](const std::filesystem::path& p) {
            std::error_code ec;
            int n = 0;
            if (!std::filesystem::is_directory(p, ec)) return 0;
            for (std::filesystem::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) ++n;
            return n;
        };
        QString s = QString("<b>%1</b> · 需要 Java %2 · mods %3 · 存档 %4")
                        .arg(id)
                        .arg(java)
                        .arg(count(dir / "mods"))
                        .arg(count(dir / "saves"));
        s += QString("<br>%1").arg(QString::fromStdString(dir.string()));
        verSummary_->setText(s);
        // 版本管理里选中的版本 = Mod 管理卡片 / JVM 设置的目标版本
        if (modVersionCombo_) {
            const int idx = modVersionCombo_->findData(id);
            if (idx >= 0) modVersionCombo_->setCurrentIndex(idx);
        }
        loadVersionJvmSettings(id);
        if (instModTable_) refreshInstalledMods();
    }

    void loadVersionJvmSettings(const QString& id) {
        versionSettingsId_ = id.toStdString();
        loadingVersionSettings_ = true;
        const core::VersionOptions opt = cfg_.optionsFor(versionSettingsId_);
        if (extraArgsEdit_) {
            QStringList args;
            for (const auto& a : opt.extraJvmArgs) args << QString::fromStdString(a);
            extraArgsEdit_->setText(args.join(' '));
        }
        if (langCombo_) {
            const int i = langCombo_->findData(QString::fromStdString(opt.language));
            langCombo_->setCurrentIndex(i >= 0 ? i : 0);
        }
        if (gameDisplay_) {
            const int i = gameDisplay_->findData(QString::fromStdString(opt.gameDisplay));
            gameDisplay_->setCurrentIndex(i >= 0 ? i : 0);
        }
        loadingVersionSettings_ = false;
    }

    void saveVersionJvmOptions() {
        if (loadingVersionSettings_ || versionSettingsId_.empty()) return;
        core::VersionOptions opt = cfg_.optionsFor(versionSettingsId_);
        opt.extraJvmArgs.clear();
        if (extraArgsEdit_) {
            const QString extra = extraArgsEdit_->text().trimmed();
            if (!extra.isEmpty())
                for (const QString& s : extra.split(' ', Qt::SkipEmptyParts))
                    opt.extraJvmArgs.push_back(s.toStdString());
        }
        if (langCombo_) opt.language = langCombo_->currentData().toString().toStdString();
        if (gameDisplay_) opt.gameDisplay = gameDisplay_->currentData().toString().toStdString();
        cfg_.versionOptions[versionSettingsId_] = opt;
        saveConfigQuietly();
    }

    QWidget* pageDirs() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "游戏目录");

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

    // ---- 资源：世界 / 资源包 / 光影 ----
    std::filesystem::path currentResDir() const {
        const auto base = core::isolatedGameDir(layout_, cfg_);
        const int idx = resTabs_ ? resTabs_->currentIndex() : 0;
        if (idx == 1) return base / "resourcepacks";
        if (idx == 2) return base / "shaderpacks";
        if (idx == 3) return core::trashRoot(layout_);   // 回收站是全局的，不按版本分
        return base / "saves";
    }

    QTableWidget* makeResTable(QWidget* parent, const QStringList& headers) {
        auto* t = new QTableWidget(0, headers.size(), parent);
        t->setHorizontalHeaderLabels(headers);
        t->horizontalHeader()->setStretchLastSection(true);
        t->verticalHeader()->setVisible(false);
        t->setShowGrid(false);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->setSelectionMode(QAbstractItemView::ExtendedSelection);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        return t;
    }

    void fillResTable(QTableWidget* table, const std::filesystem::path& dir, bool onlyDirs) {
        if (!table) return;
        table->setRowCount(0);
        for (const auto& e : core::listEntries(dir)) {
            if (onlyDirs && !e.isDir) continue;
            if (!onlyDirs && e.isDir) continue;
            const int r = table->rowCount();
            table->insertRow(r);
            auto* nameItem = new QTableWidgetItem(QString::fromStdString(e.name));
            nameItem->setData(Qt::UserRole, QString::fromStdString(e.path.string()));
            table->setItem(r, 0, nameItem);
            table->setItem(r, 1, new QTableWidgetItem(e.enabled ? "启用" : "已禁用"));
            table->setItem(r, 2, new QTableWidgetItem(
                e.isDir ? "目录" : QString::number(e.size / 1024.0, 'f', 0) + " KB"));
        }
    }

    void fillTrashTable(QTableWidget* table) {
        if (!table) return;
        table->setRowCount(0);
        for (const auto& t : core::listTrash(core::trashRoot(layout_))) {
            const int r = table->rowCount();
            table->insertRow(r);
            auto* nameItem = new QTableWidgetItem(QString::fromStdString(t.name));
            nameItem->setData(Qt::UserRole, QString::fromStdString(t.payload.string()));
            nameItem->setToolTip(QString("原位置：%1").arg(QString::fromStdString(t.original.string())));
            table->setItem(r, 0, nameItem);
            table->setItem(r, 1, new QTableWidgetItem(QString::fromStdString(t.when)));
            table->setItem(r, 2, new QTableWidgetItem(
                QString::fromStdString(mc263::fsx::humanSize(t.size))));
        }
    }

    void refreshResources() {
        if (!resTabs_) return;
        const auto base = core::isolatedGameDir(layout_, cfg_);
        fillResTable(savesTable_, base / "saves", true);
        fillResTable(packsTable_, base / "resourcepacks", false);
        fillResTable(shadersTable_, base / "shaderpacks", false);
        fillTrashTable(trashTable_);
        if (resState_) {
            const auto trashItems = core::listTrash(core::trashRoot(layout_));
            const uint64_t trashBytes = core::trashSize(core::trashRoot(layout_));
            QString line = QString("当前版本目录：%1（世界 %2 / 资源包 %3 / 光影 %4）")
                               .arg(QString::fromStdString(base.string()))
                               .arg(savesTable_ ? savesTable_->rowCount() : 0)
                               .arg(packsTable_ ? packsTable_->rowCount() : 0)
                               .arg(shadersTable_ ? shadersTable_->rowCount() : 0);
            if (!trashItems.empty())
                line += QString("；回收站 %1 项（%2）")
                            .arg(trashItems.size())
                            .arg(QString::fromStdString(mc263::fsx::humanSize(trashBytes)));
            resState_->setText(line);
        }
    }

    QTableWidget* currentResTable() const {
        if (!resTabs_) return nullptr;
        const int idx = resTabs_->currentIndex();
        if (idx == 1) return packsTable_;
        if (idx == 2) return shadersTable_;
        if (idx == 3) return trashTable_;
        return savesTable_;
    }

    QWidget* pageResources() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "资源",
                      "世界 / 资源包 / 光影都在当前版本目录下：可导入、启用禁用、删除；"
                      "下载资源包与光影请到「资源下载」页");

        resTabs_ = new QTabWidget(w);
        savesTable_ = makeResTable(w, {"世界名", "状态", "大小"});
        packsTable_ = makeResTable(w, {"资源包", "状态", "大小"});
        shadersTable_ = makeResTable(w, {"光影包", "状态", "大小"});
        trashTable_ = makeResTable(w, {"名称", "删除时间", "大小"});
        resTabs_->addTab(savesTable_, "世界");
        resTabs_->addTab(packsTable_, "资源包");
        resTabs_->addTab(shadersTable_, "光影");
        resTabs_->addTab(trashTable_, "回收站");

        v->addWidget(resTabs_, 1);

        auto* actions = new QWidget(w);
        auto* row = new QHBoxLayout(actions);
        row->setContentsMargins(0, 0, 0, 0);
        auto* refresh = new QPushButton("刷新", w);
        auto* toggle = new QPushButton("启用 / 禁用", w);
        auto* del = new QPushButton("删除", w);
        auto* restoreBtn = new QPushButton("恢复", w);
        auto* importBtn = new QPushButton("导入…", w);
        auto* openBtn = new QPushButton("打开目录", w);
        auto* emptyBtn = new QPushButton("清空回收站", w);
        restoreBtn->setToolTip("把回收站里选中的条目放回原位置；目标已存在时自动加序号，不覆盖");
        emptyBtn->setToolTip("彻底删除回收站里的全部条目，删掉不可恢复");
        row->addWidget(refresh);
        row->addWidget(toggle);
        row->addWidget(del);
        row->addWidget(restoreBtn);
        row->addWidget(importBtn);
        row->addWidget(openBtn);
        row->addStretch(1);
        row->addWidget(emptyBtn);
        v->addWidget(actions);
        resState_ = new QLabel("", w);
        resState_->setObjectName("muted");
        v->addWidget(resState_);

        connect(refresh, &QPushButton::clicked, this, &LauncherWindow::refreshResources);
        connect(resTabs_, &QTabWidget::currentChanged, this, [this](int) { refreshResources(); });
        connect(toggle, &QPushButton::clicked, this, [this] {
            if (resTabs_->currentIndex() == 0) { resState_->setText("世界不能禁用，只能删除或换目录"); return; }
            if (resTabs_->currentIndex() == 3) { resState_->setText("回收站里的条目不能启用 / 禁用，先恢复再操作"); return; }
            auto* t = currentResTable();
            if (!t || !t->selectionModel()) return;
            const auto rows = t->selectionModel()->selectedRows();
            int changed = 0;
            for (const auto& idx : rows) {
                auto* nameItem = t->item(idx.row(), 0);
                auto* st = t->item(idx.row(), 1);
                if (!nameItem) continue;
                const bool enabled = st && st->text() == "启用";
                std::string err;
                if (core::setEntryEnabled(
                        std::filesystem::path(nameItem->data(Qt::UserRole).toString().toStdString()),
                        !enabled, err))
                    ++changed;
            }
            refreshResources();
            resState_->setText(QString("已切换 %1 个（.disabled = 游戏不加载）").arg(changed));
        });
        connect(del, &QPushButton::clicked, this, [this] {
            auto* t = currentResTable();
            if (!t || !t->selectionModel()) return;
            const auto rows = t->selectionModel()->selectedRows();
            if (rows.isEmpty()) { resState_->setText("先在列表里选一个"); return; }
            const bool inTrash = resTabs_->currentIndex() == 3;
            const QString ask =
                inTrash ? QString("彻底删除选中的 %1 项？回收站里删掉就找不回来了").arg(rows.size())
                        : QString("删除选中的 %1 项？会移到回收站，可在「回收站」标签里恢复").arg(rows.size());
            if (QMessageBox::question(this, inTrash ? "彻底删除" : "删除", ask,
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;
            const auto trashItems = inTrash ? core::listTrash(core::trashRoot(layout_))
                                            : std::vector<core::TrashItem>{};
            int done = 0;
            for (const auto& idx : rows) {
                auto* nameItem = t->item(idx.row(), 0);
                if (!nameItem) continue;
                const auto p = std::filesystem::path(nameItem->data(Qt::UserRole).toString().toStdString());
                if (inTrash) {
                    for (const auto& it : trashItems) {
                        if (it.payload != p) continue;
                        std::error_code ec;
                        done += static_cast<int>(std::filesystem::remove_all(it.dir, ec));
                        break;
                    }
                } else {
                    std::string err;
                    std::filesystem::path moved;
                    if (core::moveToTrash(p, core::trashRoot(layout_), moved, err))
                        ++done;
                    else
                        resState_->setText("删除失败：" + QString::fromStdString(err));
                }
            }
            refreshResources();
            resState_->setText(inTrash ? QString("已彻底删除 %1 项").arg(done)
                                       : QString("已移到回收站 %1 项（「回收站」标签可恢复）").arg(done));
        });
        connect(restoreBtn, &QPushButton::clicked, this, [this] {
            if (resTabs_->currentIndex() != 3) {
                resState_->setText("「恢复」只对回收站里的条目有效");
                return;
            }
            if (!trashTable_ || !trashTable_->selectionModel()) return;
            const auto rows = trashTable_->selectionModel()->selectedRows();
            if (rows.isEmpty()) { resState_->setText("先在回收站里选一个"); return; }
            const auto items = core::listTrash(core::trashRoot(layout_));
            int done = 0;
            QString lastErr;
            for (const auto& idx : rows) {
                auto* nameItem = trashTable_->item(idx.row(), 0);
                if (!nameItem) continue;
                const auto p = std::filesystem::path(nameItem->data(Qt::UserRole).toString().toStdString());
                for (const auto& it : items) {
                    if (it.payload != p) continue;
                    std::string err;
                    if (core::restoreTrash(it, err)) ++done;
                    else lastErr = QString::fromStdString(err);
                    break;
                }
            }
            refreshResources();
            resState_->setText(lastErr.isEmpty() ? QString("已恢复 %1 项").arg(done)
                                                 : QString("恢复 %1 项，另有失败：%2").arg(done).arg(lastErr));
        });
        connect(emptyBtn, &QPushButton::clicked, this, [this] {
            const auto root = core::trashRoot(layout_);
            const auto items = core::listTrash(root);
            if (items.empty()) { resState_->setText("回收站已经是空的"); return; }
            const uint64_t bytes = core::trashSize(root);
            const QString ask = QString("彻底清空回收站？%1 项，共 %2，删掉不可恢复")
                                    .arg(items.size())
                                    .arg(QString::fromStdString(mc263::fsx::humanSize(bytes)));
            if (QMessageBox::question(this, "清空回收站", ask, QMessageBox::Yes | QMessageBox::No,
                                      QMessageBox::No) != QMessageBox::Yes)
                return;
            uint64_t freed = 0;
            const int n = core::emptyTrash(root, freed);
            refreshResources();
            resState_->setText(QString("已清空 %1 项，释放 %2")
                                   .arg(n)
                                   .arg(QString::fromStdString(mc263::fsx::humanSize(freed))));
        });
        connect(importBtn, &QPushButton::clicked, this, [this] {
            if (resTabs_->currentIndex() == 3) {
                resState_->setText("回收站里不能导入，先切回世界 / 资源包 / 光影");
                return;
            }
            const auto target = currentResDir();
            std::error_code ec;
            std::filesystem::create_directories(target, ec);
            if (resTabs_->currentIndex() == 0) {
                const QString dir = QFileDialog::getExistingDirectory(this, "选择世界目录（level.dat 所在目录）");
                if (dir.isEmpty()) return;
                const auto dest = target / std::filesystem::path(dir.toStdString()).filename();
                std::filesystem::copy(dir.toStdString(), dest,
                                      std::filesystem::copy_options::recursive |
                                          std::filesystem::copy_options::overwrite_existing, ec);
                resState_->setText(ec ? "导入失败：" + QString::fromStdString(ec.message())
                                      : "已导入世界 " + QString::fromStdString(dest.filename().string()));
            } else {
                const QString file = QFileDialog::getOpenFileName(this, "选择 zip", QString(), "资源包/光影 (*.zip)");
                if (file.isEmpty()) return;
                const auto dest = target / std::filesystem::path(file.toStdString()).filename();
                std::filesystem::copy_file(file.toStdString(), dest,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                resState_->setText(ec ? "导入失败：" + QString::fromStdString(ec.message())
                                      : "已导入 " + QString::fromStdString(dest.filename().string()));
            }
            refreshResources();
        });
        connect(openBtn, &QPushButton::clicked, this, [this] {
            const auto dir = currentResDir();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            QProcess::startDetached("xdg-open", {QString::fromStdString(dir.string())});
        });

        refreshResources();
        return w;
    }

    QWidget* pageSettings() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        addPageHeader(v, "支持", "外观与关于 / 许可信息");
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
            QMetaObject::invokeMethod(qApp, [guard, ok, err, list, loader] {
                if (guard) guard->applyLoaderVersions(ok, err, list, loader);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void applyLoaderVersions(bool ok, const std::string& err,
                             const std::vector<core::LoaderVersion>& list,
                             const std::string& loader) {
        loaderVerCombo_->clear();
        loaderVerCombo_->setEnabled(false);
        if (!ok || list.empty()) {
            loaderState_->setText("获取失败：" + QString::fromStdString(err));
            return;
        }
        int latestIdx = -1;
        for (int i = 0; i < static_cast<int>(list.size()); ++i)
            if (list[static_cast<size_t>(i)].stable) { latestIdx = i; break; }
        if (latestIdx < 0) latestIdx = 0;
        const auto& latest = list[static_cast<size_t>(latestIdx)];
        loaderVerCombo_->addItem(
            QString::fromStdString(latest.version) + (latest.stable ? "（稳定）" : ""),
            QString::fromStdString(latest.version));
        loaderVerCombo_->setCurrentIndex(0);
        loaderState_->setText(QString("已固定最新%1版 %2")
                                  .arg(latest.stable ? "稳定" : "预览",
                                       QString::fromStdString(latest.version)));
        if (loaderAutoUpdating_) {
            loaderState_->setText("正在自动更新加载器…");
            return;
        }
        const std::string activeLoader =
            !cfg_.profile.empty() ? cfg_.profile : (cfg_.fabric ? "fabric" : "");
        if (activeLoader != loader) return;

        const std::string installed =
            core::installedLoaderVersion(layout_, cfg_.version, loader);
        int installedIdx = -1;
        for (int i = 0; i < static_cast<int>(list.size()); ++i)
            if (list[static_cast<size_t>(i)].version == installed) { installedIdx = i; break; }
        if (!installed.empty() && installedIdx == latestIdx) {
            loaderState_->setText(QString("已是最新%1版 %2")
                                      .arg(latest.stable ? "稳定" : "预览",
                                           QString::fromStdString(latest.version)));
            return;
        }
        // API 按新→旧排列：已装的比最新稳定版还新（例如手动装过 beta）就不降级
        if (!installed.empty() && installedIdx >= 0 && installedIdx < latestIdx) return;
        startLoaderInstall(loader, latest.version, true);
    }

    void installSelectedLoader() {
        if (loaderVerCombo_->count() == 0) { loaderState_->setText("先点「获取版本」"); return; }
        const std::string loader = loaderCombo_->currentData().toString().toStdString();
        const std::string lv = loaderVerCombo_->currentData().toString().toStdString();
        startLoaderInstall(loader, lv, false);
    }

    void startLoaderInstall(const std::string& loader, const std::string& lv, bool autoUpdate) {
        core::Config c = cfg_;
        core::Layout l = layout_;
        loaderAutoUpdating_ = autoUpdate;
        installLoaderBtn_->setEnabled(false);
        loaderCombo_->setEnabled(false);
        loaderVerCombo_->setEnabled(false);
        loaderState_->setText(autoUpdate
                                  ? QString("发现新版本，自动更新到 %1…").arg(QString::fromStdString(lv))
                                  : "安装中…（会下载加载器依赖）");
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, l, c, loader, lv, autoUpdate]() mutable {
            std::string err;
            const bool ok = core::installLoader(l, c, loader, lv, nullptr, err);
            QMetaObject::invokeMethod(qApp, [guard, ok, err, c, loader, lv, autoUpdate] {
                if (!guard) return;
                guard->loaderAutoUpdating_ = false;
                guard->installLoaderBtn_->setEnabled(true);
                guard->loaderCombo_->setEnabled(true);
                guard->loaderVerCombo_->setEnabled(false);
                if (!ok) {
                    guard->loaderState_->setText(
                        (autoUpdate ? "自动更新失败：" : "失败：") + QString::fromStdString(err));
                    return;
                }
                guard->cfg_ = c;
                std::string serr;
                guard->cfg_.save(guard->layout_.configFile, serr);
                if (guard->launchVersion_)
                    guard->launchVersion_->setCurrentText(
                        QString::fromStdString(core::versionDirName(c)));
                guard->loaderState_->setText(
                    (autoUpdate ? QString("已自动更新加载器 %1 %2 → %3")
                                  : QString("已装 %1 %2 → %3"))
                        .arg(QString::fromStdString(loader), QString::fromStdString(lv),
                             QString::fromStdString(core::versionDirName(c))));
                guard->modLog_->appendPlainText(
                    QString("[%1] 加载器 %2 %3 %4")
                        .arg(QTime::currentTime().toString("HH:mm:ss"),
                             QString::fromStdString(loader), QString::fromStdString(lv),
                             autoUpdate ? "已自动更新" : "安装完成"));
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
            "许可AGPL-3.0-or-later\n"
            "Qt6 运行时:LGPL-3.0-only OR GPL-3.0-only WITH Qt-GPL-exception-1.0（动态链接）\n"
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
        docRow->addWidget(new QLabel("README", w));
        if (readme.isEmpty()) {
            auto* miss = new QLabel("未找到 README", w);
            miss->setStyleSheet("color:" + (dark ? QStringLiteral("#9aa4b2")
                                                  : QStringLiteral("#5b6472")) + ";");
            docRow->addWidget(miss, 1);
        } else {
            auto* docLink = new QLabel(w);
            docLink->setTextFormat(Qt::RichText);
            docLink->setText(QStringLiteral("<a href=\"%1\" style=\"color:%2;\">README</a>")
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
        link->setToolTip("你看什么");
        repoRow->addWidget(new QLabel("仓库：", w));
        repoRow->addWidget(link, 1);
        v->addLayout(repoRow);

        // 实际生效的显示后端（Wayland 原生 / XWayland / offscreen），方便排查
        const QString plat = QGuiApplication::platformName();
        QString platNote = plat;
        if (plat.startsWith("Wayland")) platNote += "";
        else if (plat == "xcb") platNote += "X11 / XWayland";
        auto* platLabel = new QLabel("显示后端：" + platNote + " * Qt " +
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
        memSummary_->setText(QString("物理内存 <b>%1</b> · 其他程序占用 <b>%2</b>(%3%)")
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
                           ? QString("JVM 自动：预计上限约 %1(不传 -Xmx)").arg(fmtMem(static_cast<uint64_t>(want)))
                           : QString("-Xmx%1M * 占物理内存 %2% * 占当前可用 %3%")
                                 .arg(want).arg(pTotal).arg(pAvail);
        if (avail) {
            const uint64_t left = static_cast<uint64_t>(want) < avail
                                      ? avail - static_cast<uint64_t>(want) : 0;
            line += " * 启动后预计剩余 " + fmtMem(left);
        }
        if (autoMode) {
            memPreview_->hide();
        } else {
            memPreview_->setText(line);
            memPreview_->show();
        }
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
            memWarn_->setText(QString("注意：超出当前可用内存 %1(仅可用 %2)")
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
        if (idx == 2) {                            // Mod 页：刷新版本下拉与加载器
            refreshModVersionChoices();
            refreshLoaderVersions();
        }
        else if (idx == 3) {                       // 版本管理：版本列表 + 已装 Mod
            refreshVersionList();
            refreshInstalledMods();
        }
        else if (idx == 5) refreshResources();    // 资源页
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
        lastPalette_ = qApp->palette();   // changeEvent 里用它判断"是不是自己设的"
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
            txt = QStringLiteral("读的是 %1(%2)")
                      .arg(QString::fromStdString(m3_.path),
                           m3_.dark() ? QStringLiteral("深色") : QStringLiteral("浅色"));
        else
            txt = QStringLiteral("没找到 Noctalia 色板(会依次找 %1)")
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
        launchVersion_->setCurrentText(QString::fromStdString(core::versionDirName(cfg_)));
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
        if (themeCombo_) {
            const int ti = themeCombo_->findData(QString::fromStdString(cfg_.themeSource));
            themeCombo_->setCurrentIndex(ti >= 0 ? ti : 0);
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
        std::string selected = launchVersion_->currentText().toStdString();
        if (selected.empty()) selected = "26.3";
        // 下拉框里可能直接是加载器目录（26.3+fabric）：拆成 base + profile，
        // 避免 versionDirName 再拼一次变成 26.3+fabric+fabric
        if (const auto plus = selected.rfind('+'); plus != std::string::npos) {
            cfg_.profile = selected.substr(plus + 1);
            selected.resize(plus);
            cfg_.fabric = (cfg_.profile == "fabric");
        } else {
            cfg_.profile.clear();
            cfg_.fabric = false;
        }
        cfg_.version = selected;
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
        if (themeCombo_) cfg_.themeSource = themeCombo_->currentData().toString().toStdString();
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
        connect(launchVersion_, &QComboBox::currentTextChanged, this, &LauncherWindow::scheduleAutoSave);
        connect(mirrorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &LauncherWindow::scheduleAutoSave);
        connect(jobsSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &LauncherWindow::scheduleAutoSave);
        connect(memSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &LauncherWindow::scheduleAutoSave);
        connect(xmsSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
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
        const QString active = QString::fromStdString(core::versionDirName(cfg_));
        if (cur.isEmpty() && !cfg_.version.empty()) launchVersion_->addItem(active);
        if (!cur.isEmpty()) launchVersion_->setCurrentText(cur);
        else launchVersion_->setCurrentText(active);
        launchState_->setText("本地版本 " + QString::number(vers.size()) + " 个");
        refreshVersionList();
    }
    // 取 Mojang 的 latest.snapshot（预览版），填进版本框，让用户自己点开始下载
    void fetchLatestSnapshot() {
        collectUiToConfig();
        const core::Layout l = layout_;
        const std::string mirror = cfg_.mirror;
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, l, mirror] {
            std::string err;
            const std::string id = core::latestSnapshot(mirror, l.configDir / "cache", err);
            QMetaObject::invokeMethod(qApp, [guard, id, err] {
                if (!guard) return;
                if (id.empty()) {
                    guard->downloadLog_->appendPlainText(
                        "拿不到最新预览版: " + QString::fromStdString(err));
                } else {
                    guard->downloadVersion_->setCurrentText(QString::fromStdString(id));
                    guard->downloadLog_->appendPlainText("最新预览版: " + QString::fromStdString(id));
                }
            }, Qt::QueuedConnection);
        }).detach();
    }

    void refreshOnlineVersions() {
        collectUiToConfig();
        const core::Layout l = layout_;
        const std::string mirror = cfg_.mirror;
        const bool snapshots = snapCheck_ && snapCheck_->isChecked();
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, l, mirror, snapshots] {
            std::string err;
            auto vers = core::onlineVersions(mirror, l.configDir / "cache", snapshots, 0, err);
            QMetaObject::invokeMethod(qApp, [guard, vers, err] {
                if (guard) guard->applyOnlineVersions(vers, QString::fromStdString(err));
            }, Qt::QueuedConnection);
        }).detach();
    }

    void refreshJava() {
        javaState_->setText("后台扫描 Java 中......");
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
        compatState_->setText("后台查验中......");
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
                                         "] 缓存已是空的 : " + QString::fromStdString(cache.string()));
            downloadLabel_->setText("缓存为空");
            return;
        }
        const QString msg = QString("将删除 %1 个缓存文件(%2):\n%3\n\n只清元数据缓存,已下载的游戏文件(jar / libraries / assets)不受影响...吗？")
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
        downloadLabel_->setText(QString("缓存已清除(%1)").arg(fmtMem(bytes / (1024 * 1024))));
    }

    void startDownload() {
        if (downloadRunning_) {
            QMessageBox::information(this, "正在下载", "下载任务已经在运行");
            return;
        }
        collectUiToConfig();
        std::string err;
        cfg_.save(layout_.configFile, err);
        core::Config job = cfg_;
        job.version = downloadVersion_->currentText().toStdString();
        downloadLog_->clear();
        downloadLog_->appendPlainText("启动下载中（" + QString::number(job.jobs) + " 线程）......");
        startDownloadJob(job, {}, false);
    }

    // 校验已装文件：只信本地 <版本>.json，缺/坏的文件由下载器补回，不重新拉清单
    void verifyInstalled() {
        if (downloadRunning_) {
            QMessageBox::information(this, "正在运行", "下载或校验任务已经在运行");
            return;
        }
        collectUiToConfig();
        std::string err;
        cfg_.save(layout_.configFile, err);
        const std::string ver = core::baseGameVersion(downloadVersion_->currentText().toStdString());
        if (!mc263::fsx::isFile(layout_.versionJson(ver))) {
            downloadLog_->clear();
            downloadLog_->appendPlainText("本地没有 " + QString::fromStdString(ver) +
                                          " 的版本 json，先下载这个版本再校验。");
            downloadLabel_->setText("这个版本还没下载过");
            return;
        }
        core::Config job = cfg_;
        job.version = ver;
        job.components = {"jar", "libraries", "assets"};
        core::DownloadOptions opt;
        opt.mode = core::DownloadMode::Verify;
        downloadLog_->clear();
        downloadLog_->appendPlainText("开始校验 " + QString::fromStdString(ver) + " ...");
        startDownloadJob(job, opt, true);
    }

    // worker 线程只通过 queued 回调碰 UI；generation 用来丢弃取消后迟到的旧任务事件。
    void startDownloadJob(core::Config job, core::DownloadOptions opt, bool verifyMode) {
        const quint64 gen = ++downloadGeneration_;
        auto token = std::make_shared<std::atomic_bool>(false);
        downloadCancel_ = token;
        downloadRunning_ = true;
        downloadBtn_->setEnabled(false);
        cancelBtn_->setEnabled(true);
        downloadBar_->setRange(0, 0);
        downloadLabel_->setText(verifyMode ? "校验中..." : "下载中...");
        const core::Layout l = layout_;
        QPointer<LauncherWindow> guard(this);
        std::thread([guard, l, job, opt, token, gen, verifyMode] {
            struct Throttle {
                std::chrono::steady_clock::time_point last{};
                std::string stage;
            };
            auto throttle = std::make_shared<Throttle>();
            auto progress = [guard, gen, throttle](const core::DownloadProgress& p) {
                const auto now = std::chrono::steady_clock::now();
                const bool force = !p.ok || p.done == p.total || p.stage == "summary" ||
                                   p.stage == "done" || p.stage != throttle->stage;
                if (!force && now - throttle->last < std::chrono::milliseconds(100)) return;
                throttle->last = now;
                throttle->stage = p.stage;
                QMetaObject::invokeMethod(qApp, [guard, gen, p] {
                    if (guard && guard->downloadGeneration_ == gen)
                        guard->applyDownloadProgress(p);
                }, Qt::QueuedConnection);
            };
            core::DownloadResult res;
            try {
                res = core::downloadAll(l, job, progress, opt,
                    [token] { return token->load(); });
            } catch (const std::exception& ex) {
                res.error = std::string("下载线程异常: ") + ex.what();
            } catch (...) {
                res.error = "下载线程未知异常";
            }
            QMetaObject::invokeMethod(qApp, [guard, res, gen, verifyMode] {
                if (guard) guard->finishDownloadJob(res, verifyMode, gen);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void applyDownloadProgress(const core::DownloadProgress& p) {
        if (p.stage == "summary") {
            const QString text = QString::fromStdString(p.label);
            downloadLog_->appendPlainText(text);
            downloadLabel_->setText(text);
            return;
        }
        if (p.stage == "done") {
            downloadLog_->appendPlainText(p.ok ? "全部完成" : "部分文件失败");
            return;
        }
        if (p.total > 0) {
            downloadBar_->setRange(0, static_cast<int>(p.total));
            downloadBar_->setValue(static_cast<int>(p.done));
        }
        const QString label = QString::fromStdString(p.label);
        const QString note = QString::fromStdString(p.note);
        downloadLabel_->setText(QString::fromStdString(p.stage) + "  " +
                                QString::number(p.done) + "/" + QString::number(p.total) +
                                "  " + label);
        if (p.done % 50 == 0 || !p.ok)
            downloadLog_->appendPlainText(QString::fromStdString(p.stage) + " " +
                                          QString::number(p.done) + "/" + QString::number(p.total) +
                                          " " + label + (p.ok ? "" : " 失败: " + note));
    }

    void finishDownloadJob(const core::DownloadResult& r, bool verifyMode, quint64 gen) {
        if (gen != downloadGeneration_) return;
        downloadRunning_ = false;
        if (downloadCancel_) downloadCancel_.reset();
        downloadBtn_->setEnabled(true);
        cancelBtn_->setEnabled(false);
        downloadBar_->setRange(0, 1);
        downloadBar_->setValue(1);
        if (r.cancelled) {
            downloadLabel_->setText("已取消");
            downloadLog_->appendPlainText("任务已取消，未完成的 .part 文件已清理。");
            return;
        }
        if (!r.ok) {
            downloadLabel_->setText(verifyMode ? "校验结束" : "下载结束");
            downloadLog_->appendPlainText("失败: " + QString::fromStdString(r.error) +
                                          "（失败文件 " + QString::number(r.failed) + " 个）");
            return;
        }
        if (verifyMode) {
            downloadLabel_->setText(r.bad > 0
                                        ? "校验完成，修复 " + QString::number(r.bad) + " 项"
                                        : "校验完成，全部正常");
            downloadLog_->appendPlainText(QString("校验完成：检查 %1 项，异常 %2，补下 %3，缓存 %4")
                                              .arg(r.checked).arg(r.bad).arg(r.downloaded).arg(r.cached));
        } else {
            downloadLabel_->setText("下载完成");
            downloadLog_->appendPlainText(QString("下载完成：新下载 %1，缓存命中 %2")
                                              .arg(r.downloaded).arg(r.cached));
        }
        refreshLocalVersions();
        if (!verifyMode) refreshJava();  // 运行时下载完可能就有 Java 25 了，重新过一遍闸门
    }

    // 崩溃摘要预览：复制走剪贴板，完整内容留在 note/ 里
    void showCrashReport(const std::filesystem::path& path) {
        std::string rerr;
        const QString text = QString::fromStdString(mc263::fsx::readText(path, rerr));
        if (!rerr.empty()) {
            launchLog_->appendPlainText("读不了崩溃摘要：" + QString::fromStdString(rerr));
            return;
        }
        QDialog dlg(this);
        dlg.setWindowTitle("游戏异常退出");
        dlg.resize(780, 540);
        auto* v = new QVBoxLayout(&dlg);
        auto* head = new QLabel(QString("已生成崩溃摘要：%1").arg(QString::fromStdString(path.string())), &dlg);
        head->setWordWrap(true);
        head->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(head);
        auto* edit = new QPlainTextEdit(&dlg);
        edit->setReadOnly(true);
        edit->setPlainText(text.size() > 20000
                               ? text.left(20000) + "\n…（预览截断，完整内容见上面的文件）"
                               : text);
        v->addWidget(edit, 1);
        auto* row = new QHBoxLayout();
        auto* copyBtn = new QPushButton("复制摘要", &dlg);
        auto* openBtn = new QPushButton("打开目录", &dlg);
        auto* closeBtn = new QPushButton("关闭", &dlg);
        row->addWidget(copyBtn);
        row->addWidget(openBtn);
        row->addStretch(1);
        row->addWidget(closeBtn);
        v->addLayout(row);
        connect(copyBtn, &QPushButton::clicked, &dlg, [edit] {
            QApplication::clipboard()->setText(edit->toPlainText());
        });
        connect(openBtn, &QPushButton::clicked, &dlg, [path] {
            // 用 QDesktopServices 而不是 xdg-open：Ubuntu / Arch 都装了实现，
            // 少了 xdg-utils 也不会静默失败
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QString::fromStdString(path.parent_path().string())));
        });
        connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
        dlg.exec();
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
        // 当前版本的 JVM / 语言 / 显示后端：按版本目录隔离，没设置过则回退全局默认
        const std::string optId = core::versionDirName(cfg_);
        core::VersionOptions vopt = cfg_.optionsFor(optId);
        core::Config run = cfg_;
        run.extraJvmArgs = vopt.extraJvmArgs;
        run.language = vopt.language;
        run.gameDisplay = vopt.gameDisplay;
        run.appliedLanguage = vopt.appliedLanguage;
        // 游戏以 options.txt 的 lang: 为准（--language 只是初始默认），
        // 所以启动器设置变了就把它写进去；设置没变则不动，尊重游戏里改过的语言。
        QString langNotice;
        const std::string wantLang = core::effectiveGameLanguage(run);
        if (wantLang != run.appliedLanguage) {
            std::string lerr;
            if (core::applyGameLanguage(
                    layout_.versionsDir / core::effectiveVersionId(layout_, cfg_), wantLang, lerr)) {
                vopt.appliedLanguage = wantLang;
                cfg_.versionOptions[optId] = vopt;
                std::string serr;
                cfg_.save(layout_.configFile, serr);
                langNotice = "游戏语言 → " + QString::fromStdString(wantLang);
            } else {
                langNotice = "写 options.txt 语言失败：" + QString::fromStdString(lerr);
            }
        }
        QString fallbackNotice;
        if (core::effectiveVersionId(layout_, run) != core::versionDirName(run)) {
            fallbackNotice = QString("找不到 %1，本次改用原版 %2 启动")
                                 .arg(QString::fromStdString(core::versionDirName(run)),
                                      QString::fromStdString(core::effectiveVersionId(layout_, run)));
        }
        core::LaunchPlan plan = core::buildLaunchPlan(layout_, run, java);
        if (!plan.ok()) {
            QMessageBox::warning(this, "无法启动", QString::fromStdString(plan.error));
            return;
        }
        launchLog_->clear();
        if (!langNotice.isEmpty()) launchLog_->appendPlainText(langNotice);
        if (!fallbackNotice.isEmpty()) launchLog_->appendPlainText(fallbackNotice);
        launchLog_->appendPlainText("Java: " + QString::fromStdString(java.path.string()));
        launchLog_->appendPlainText("主类: " + QString::fromStdString(plan.mainClass));
        launchLog_->appendPlainText("classpath " + QString::number(plan.argv.size()) + " 个参数");
        // 日志落盘：<游戏目录>/note/launch-<版本>-<时间>.log，只留最近 10 个
        {
            const auto noteDir = layout_.gameDir / "note";
            std::string lerr;
            const auto logPath = core::newLogPath(noteDir, core::effectiveVersionId(layout_, cfg_), lerr);
            if (!logPath.empty()) {
                launchLogPath_ = logPath;
                launchLogFile_.setFileName(QString::fromStdString(logPath.string()));
                if (launchLogFile_.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    launchLogFile_.write(
                        QString("== %1 ==\nJava: %2\n主类: %3\n参数: %4 个\n\n")
                            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"),
                                 QString::fromStdString(java.path.string()),
                                 QString::fromStdString(plan.mainClass))
                            .arg(plan.argv.size())
                            .toUtf8());
                    launchLogFile_.flush();
                    launchLog_->appendPlainText("日志: " + QString::fromStdString(logPath.string()));
                }
            }
        }
        launchProc_ = new QProcess(this);
        launchProc_->setProcessChannelMode(QProcess::MergedChannels);
        // 游戏窗口后端：Minecraft 26.x 用 SDL3，SDL_VIDEO_DRIVER 可强制 wayland / x11
        {
            QProcessEnvironment penv = QProcessEnvironment::systemEnvironment();
            if (run.gameDisplay == "wayland") penv.insert("SDL_VIDEO_DRIVER", "wayland");
            else if (run.gameDisplay == "x11") penv.insert("SDL_VIDEO_DRIVER", "x11");
            launchProc_->setProcessEnvironment(penv);
            if (run.gameDisplay != "auto")
                launchLog_->appendPlainText("游戏显示后端：SDL_VIDEO_DRIVER=" +
                                            QString::fromStdString(run.gameDisplay));
        }
        launchProc_->setWorkingDirectory(QString::fromStdString(plan.workDir.string()));
        QStringList args;
        for (size_t i = 1; i < plan.argv.size(); ++i) args << QString::fromStdString(plan.argv[i]);
        launchProc_->setProgram(QString::fromStdString(plan.argv[0]));
        launchProc_->setArguments(args);
        connect(launchProc_, &QProcess::readyReadStandardOutput, this, [this] {
            const QByteArray chunk = launchProc_->readAllStandardOutput();
            launchLog_->appendPlainText(QString::fromUtf8(chunk).trimmed());
            if (launchLogFile_.isOpen()) { launchLogFile_.write(chunk); launchLogFile_.flush(); }
        });
        connect(launchProc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            launchState_->setText("game exited (" + QString::number(code) + ")");
            if (launchLogFile_.isOpen()) launchLogFile_.close();
            const int removed = core::pruneLogs(layout_.gameDir / "note", 10);
            if (removed > 0)
                launchLog_->appendPlainText(QString("已清理 %1 个旧日志（最多保留 10 个）").arg(removed));
            if (code == 0) return;
            // 非 0 退出当作崩溃：把本次日志、latest.log、hs_err、crash-report 拼成一份摘要
            std::string cerr;
            const auto report = core::collectCrashReport(layout_, core::effectiveVersionId(layout_, cfg_),
                                                         launchLogPath_, code, cerr);
            if (report.empty()) {
                launchLog_->appendPlainText("生成崩溃摘要失败：" + QString::fromStdString(cerr));
                return;
            }
            launchLog_->appendPlainText("崩溃摘要：" + QString::fromStdString(report.string()));
            showCrashReport(report);
        });
        launchState_->setText("运行中......");
        launchProc_->start();
    }

    core::Layout layout_;
    std::filesystem::path dataRoot_;
    core::Config cfg_;
    std::vector<core::JavaInfo> javas_;
    int maxMemoryMb_ = 8192;
    bool downloadRunning_ = false;
    quint64 downloadGeneration_ = 0;
    std::shared_ptr<std::atomic_bool> downloadCancel_;
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
    QCompleter* modCompleter_ = nullptr;
    QTimer* modSuggestTimer_ = nullptr;
    QHash<QString, QStringList> modSuggestCache_;
    QHash<QString, qint64> modSuggestCacheAt_;
    QComboBox* modContentType_ = nullptr;
    QGroupBox* modLoaderBox_ = nullptr;
    QComboBox* modSort_ = nullptr;
    QComboBox* loaderCombo_ = nullptr;
    QComboBox* loaderVerCombo_ = nullptr;
    QPushButton* installLoaderBtn_ = nullptr;
    QLabel* loaderState_ = nullptr;
    bool loaderAutoUpdating_ = false;
    QComboBox* modVersionCombo_ = nullptr;
    QTableWidget* modTable_ = nullptr;
    QPushButton* modSearchBtn_ = nullptr;
    QPushButton* modInstallBtn_ = nullptr;
    QPushButton* modOpenDirBtn_ = nullptr;
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
    QComboBox* gameDisplay_ = nullptr;
    std::string versionSettingsId_;
    bool loadingVersionSettings_ = false;
    QFile launchLogFile_;
    std::filesystem::path launchLogPath_;
    QLabel* instModState_ = nullptr;
    QTableWidget* instModTable_ = nullptr;
    QTabWidget* resTabs_ = nullptr;
    QTableWidget* savesTable_ = nullptr;
    QTableWidget* packsTable_ = nullptr;
    QTableWidget* shadersTable_ = nullptr;
    QTableWidget* trashTable_ = nullptr;
    QLabel* resState_ = nullptr;

    QCheckBox* snapCheck_ = nullptr;
    QListWidget* verList_ = nullptr;
    QLabel* verSummary_ = nullptr;
    QLabel* themeHint_ = nullptr;
    QFileSystemWatcher* paletteWatch_ = nullptr;
    QPalette defaultPalette_;
    QPalette lastPalette_;   // 最近一次由本程序设下去的调色板
    core::M3Palette m3_;
    Theme theme_;
    bool roundedCorners_ = false;
    QTimer* autosaveTimer_ = nullptr;
    bool loadingConfig_ = false;
};

// 崩溃时把调用栈同时打到 stderr 和 note/launcher-crash-<时间>.log。
// 信号处理器里只做 snprintf / open / write / close，不分配内存。
static void fatalSignal(int sig) {
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    char head[96];
    const int len = std::snprintf(head, sizeof head, "\nmc263-qt: 收到信号 %d，调用栈：\n", sig);
    if (g_crashLogPath[0]) {
        const int fd = ::open(g_crashLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ::write(fd, head, static_cast<size_t>(len));
            ::backtrace_symbols_fd(frames, n, fd);
            ::close(fd);
        }
    }
    ::write(2, head, static_cast<size_t>(len));
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
                     "  或在有 X11/XWayland 时指定：'QT_QPA_PLATFORM=xcb' %s\n",
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
