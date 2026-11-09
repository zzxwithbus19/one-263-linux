# niri + Noctalia 适配说明

> 目标：让启动器在 **niri**（Wayland 平铺合成器）下配色跟 **Noctalia**（Quickshell 桌面外壳）
> 生成的 Material You 色板一致，并且窗口行为适合平铺桌面。
> 涉及文件：`include/launcher_core.hpp`、`src/mc263_qt.cpp`、`tests/core_test.cpp`、
> 示例规则 `packaging/niri-mc263.kdl`。

---

## 1. 背景：niri 下能用什么、不能用什么

| 事实 | 对启动器的影响 |
|------|----------------|
| niri 是平铺合成器，窗口默认占一列 | 启动器应浮动、限制宽度（见第 4 节规则） |
| niri 配置里 `prefer-no-csd`，环境里 `QT_WAYLAND_DISABLE_WINDOWDECORATION=1` | Qt 窗口没有标题栏，拖动靠 `Mod + 左键`、关闭靠 `Mod+Q` |
| niri 没有"系统强调色"这种东西 | 配色只能来自 GTK/Qt 平台主题或显式的色板文件 |
| Noctalia 会把壁纸算出的 M3 色板渲染成文件 | 启动器直接读文件，比猜 GTK 主题稳 |

所以适配拆成两件事：**读 Noctalia 色板**（程序内选项）+ **niri 窗口规则**（桌面侧配置）。

---

## 2. 色板从哪来

Noctalia 按 template 渲染 Material You 色板，本机产物在：

```
~/.cache/nyxuri/palette.toml        # 模板 ~/.config/nyxuri/templates/palette.toml
```

由 `~/.config/noctalia/noctalia-config.toml` 里的 `[theme.templates.user.palette]` 注册
（`input_path` → `output_path`）。格式是扁平 TOML，一行一个颜色：

```toml
primary = "#004890"
background = "#f9f9ff"
on_surface = "#191c21"
surface_container_low = "#f2f3fb"
…
```

搜索顺序（`core::paletteCandidates()`）：

1. `$MC263_PALETTE`（强制覆盖，调试用）
2. `$XDG_CACHE_HOME/nyxuri/palette.toml`（本机路径）
3. `$XDG_CACHE_HOME/noctalia/palette.toml`
4. `$XDG_CONFIG_HOME/noctalia/palette.toml`

只认「够用」的色板：`background`、`on_surface`、`primary` 三个都在才算有效，否则退回系统调色板。

---

## 3. 程序里的选项（设置页 → 外观）

| 选项 | 行为 |
|------|------|
| `自动`（默认） | 找到有效色板就用，找不到跟随系统调色板 |
| `跟随系统调色板` | 完全不读 Noctalia（Breeze / GTK / 自定义配色） |
| `Noctalia / Material You` | 强制读；读不到时退回系统 |

设置项存在 `config.json` 的 `themeSource`（`auto` / `system` / `noctalia`）。
选项下面有一行状态：读到的是哪个文件、深色还是浅色、当前是否 niri 会话。

**壁纸一换就自动变色**：启动器用 `QFileSystemWatcher` 盯着色板文件，Noctalia 重新渲染后
直接重设 `QPalette` + 重套 QSS，不需要重启。

---

## 4. niri 窗口规则

见 `packaging/niri-mc263.kdl`：把启动器设成浮动、宽度 55%、高度 72%。
粘进 `~/.config/niri/rules.kdl` 或 `__custom__.kdl` 后 `niri msg action reload-config`。

`app-id` 是 `QGuiApplication::setDesktopFileName("mc263-qt")` 定下的 `mc263-qt`，
规则里用 `match app-id=r"^mc263-qt$"` 命中。

---

## 5. 实现路线（改代码看这里）

| 层 | 文件 | 内容 |
|----|------|------|
| 核心（不依赖 Qt、可单测） | `include/launcher_core.hpp` | `M3Palette`、`parseM3Palette()`、`paletteCandidates()`、`loadM3Palette()`、`findM3Palette()`、`isNiriSession()`；`Config::themeSource` |
| 界面 | `src/mc263_qt.cpp` | `paletteFromM3()` 造 QPalette；`themeFor()` 所有 token 从 QPalette 推；`applyPalette()` / `applyStyleSheet()` / `updateThemeHint()` / `setupPaletteWatch()`；设置页「外观」分组 |
| 测试 | `tests/core_test.cpp` | 解析、回退、深/浅判断、`MC263_PALETTE` 优先级、niri 判定（11 项） |

启动流程：

```
cfg_.load()
  → defaultPalette_ = qApp->palette()
  → applyPalette(): findM3Palette() → paletteFromM3() → qApp->setPalette()
  → buildUi()                      // 内联颜色读 palette()
  → applyStyleSheet(): themeFor(qApp->palette()) → QSS
壁纸变 → Noctalia 重渲 palette.toml → watcher → applyPalette() + applyStyleSheet()
```

要点：**只换 QPalette，不写死颜色**。`themeFor()` 从 `QPalette::Window/Base/WindowText/Accent`
推整套 token，所以 M3 色板、Breeze、GTK 主题三种来源走同一条路径；
`MemoryBar` 等读 `palette()` 的地方也会自动跟着变。

---

## 6. 已知限制 / 下一步

- `memColors()` 里的内存条配色还是写死的蓝/橙，没跟 M3 走；要统一就同样改成 palette 推导。
- 设置页切换时立即重上色，但少数内联设置过颜色的提示标签要等对应刷新动作才会换色。
- Noctalia 若改模板变量名或路径，只需改 `paletteCandidates()` / `parseM3Palette()`。
- 想全局一致（GTK/Qt/其他 Qt 程序都跟 M3），可以让 Noctalia 再渲染 `qt6ct` / Kvantum 配置；
  启动器自己读色板更可靠，不依赖用户装 qt6ct。

---

## 7. 本机验证

```bash
make && make check      # 68 项核心单测（含 11 项色板/会话）+ 7 项下载器自测

# 浅色 / 深色色板各跑一遍冒烟（124 = 正常跑着）
QT_QPA_PLATFORM=offscreen timeout 3 build/mc263-qt; echo $?
MC263_PALETTE=~/.cache/nyxuri/palette.toml QT_QPA_PLATFORM=offscreen timeout 3 build/mc263-qt; echo $?
```
