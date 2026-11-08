# ext-background-effect-v1

从 wayland-protocols（staging）取来的协议定义：客户端向合成器申请"窗口背后模糊"。
上游 2026 年合入，niri 26.04+ / KDE Plasma 6 / GNOME（Mutter 49+）都实现了。

`src/wayland_blur.cpp` 用它把整窗区域交给合成器去模糊；不支持的桌面上只是拿不到 global，
代码自动降级，不会报错。

## 重新生成绑定

```bash
cd src/protocol
wayland-scanner client-header ext-background-effect-v1.xml ext-background-effect-client-protocol.h
wayland-scanner private-code  ext-background-effect-v1.xml ext-background-effect-protocol.c
```

生成物直接进仓库，这样构建只需要 `wayland-client`（`pkg-config wayland-client`），
不需要装 `wayland-protocols` 包。

## 更新协议

协议还在 staging，版本可能加。拉上游新 XML 覆盖本目录，重跑上面两条命令，
再把 `ext_background_effect_manager_v1_interface` 的 version 参数（`wayland_blur.cpp`
里 `wl_registry_bind` 的那一处）跟上去。
