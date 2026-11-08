#pragma once

class QWidget;

// 通过 ext-background-effect-v1 协议向合成器申请"窗口背后模糊"。
// 这是 wayland-protocols staging 里的标准协议（2026 年合入），niri 26.04+、
// KDE Plasma、GNOME（Mutter）都实现了；不支持的桌面/非 Wayland 会话下调用无副作用。
namespace mc263::blur {

// 当前是否具备条件（Wayland 会话 + 构建时带上了 wayland-client）
bool available();

// 申请（on=true）或撤销窗口的背景模糊。窗口尺寸变化、重新 show 之后都要再调一次。
void apply(QWidget* win, bool on);

// 合成器是否真的支持并已生效（用于界面提示，可在 apply 之后再查）
bool active();

}  // namespace mc263::blur
