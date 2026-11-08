#include "wayland_blur.hpp"

#include <QGuiApplication>
#include <QWidget>
#include <QWindow>

#include <cstring>

#if defined(MC263_HAVE_WAYLAND_BLUR)

#include <QtGui/qguiapplication_platform.h>
#include <wayland-client.h>

#include "protocol/ext-background-effect-client-protocol.h"

namespace {

struct State {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    ext_background_effect_manager_v1* manager = nullptr;
    ext_background_effect_surface_v1* effect = nullptr;
    wl_surface* surface = nullptr;
    int width = 0;
    int height = 0;
    bool want = false;
    bool blurOk = false;
};

State& st() {
    static State s;
    return s;
}

void dropEffect() {
    State& s = st();
    if (s.effect) {
        ext_background_effect_surface_v1_destroy(s.effect);
        s.effect = nullptr;
    }
}

// 协议是双缓冲状态：改完要 commit 一次才生效。窗口空闲时 Qt 不会自己提交，
// 所以这里补一次（此时没有待提交的 buffer，只会把状态刷上去）。
void pushRegion() {
    State& s = st();
    if (!s.effect || !s.surface || !s.compositor || s.width <= 0 || s.height <= 0) return;
    wl_region* region = wl_compositor_create_region(s.compositor);
    if (!region) return;
    wl_region_add(region, 0, 0, s.width, s.height);
    ext_background_effect_surface_v1_set_blur_region(s.effect, region);
    wl_region_destroy(region);
    wl_surface_commit(s.surface);
}

void sync() {
    State& s = st();
    if (!s.want || !s.surface) return;
    if (!s.manager || !s.compositor) return;  // 全局对象还没到，等 registry 回调再试
    if (!s.effect) s.effect = ext_background_effect_manager_v1_get_background_effect(s.manager, s.surface);
    pushRegion();
}

void onCapabilities(void*, ext_background_effect_manager_v1*, uint32_t flags) {
    st().blurOk = (flags & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR) != 0;
}

const ext_background_effect_manager_v1_listener kManagerListener = {onCapabilities};

void onGlobal(void*, wl_registry* registry, uint32_t name, const char* iface, uint32_t version) {
    State& s = st();
    if (std::strcmp(iface, wl_compositor_interface.name) == 0 && !s.compositor) {
        s.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4));
        sync();
    } else if (std::strcmp(iface, ext_background_effect_manager_v1_interface.name) == 0 && !s.manager) {
        s.manager = static_cast<ext_background_effect_manager_v1*>(
            wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1));
        if (s.manager)
            ext_background_effect_manager_v1_add_listener(s.manager, &kManagerListener, nullptr);
        sync();
    }
}

void onGlobalRemove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = {onGlobal, onGlobalRemove};

}  // namespace

namespace mc263::blur {

bool available() {
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

void apply(QWidget* win, bool on) {
    State& s = st();
    s.want = on && win;
    if (!on || !win || !available()) {
        dropEffect();
        if (s.surface) wl_surface_commit(s.surface);
        return;
    }
    auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!app) return;
    if (!s.display) s.display = app->display();
    if (!s.display) return;
    if (!s.registry) {
        s.registry = wl_display_get_registry(s.display);
        if (!s.registry) return;
        // 不主动 roundtrip：global 事件会随 Qt 的事件循环到达，到了再 sync()
        wl_registry_add_listener(s.registry, &kRegistryListener, nullptr);
    }

    QWindow* wh = win->windowHandle();
    if (!wh) return;
    // Qt 在 Wayland 下把 wl_surface 指针当 WId 暴露出来
    auto* surface = reinterpret_cast<wl_surface*>(wh->winId());
    if (!surface) return;
    if (surface != s.surface) {  // setWindowFlag/show 之后换过原生窗口
        dropEffect();
        s.surface = surface;
    }
    s.width = win->width();
    s.height = win->height();
    sync();
}

bool active() {
    return st().blurOk && st().effect != nullptr;
}

}  // namespace mc263::blur

#else  // 没装 wayland-client 开发包：退化成空实现，编译期就摘掉

namespace mc263::blur {
bool available() { return false; }
void apply(QWidget*, bool) {}
bool active() { return false; }
}  // namespace mc263::blur

#endif
