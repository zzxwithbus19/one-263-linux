# mc263 官网（`web` 分支）

GitHub Pages 用的单页站点：`index.html` + `favicon.svg`，内联 CSS/JS，无构建步骤、
无外部请求，直接当静态文件托管即可。

启动器源码不在这个分支：

| 分支 | 内容 |
|---|---|
| `main` | Arch 主线（PKGBUILD / 自建 pacman 源） |
| `fedora` | rpm 线（rpmbuild + COPR / openSUSE） |
| `web` | 本分支，只有这个页面 |

## 改哪里

- `index.html` —— 整页（样式和脚本都在里面，改完直接在浏览器刷新）
- `favicon.svg` —— 站点图标
- 页面上的数字（ELF 体积、单测项数、页面数）和功能描述以 `main` 的 `README.md` 为准；
  发版后记得同步版本号和 Releases 链接。

## 本地预览

```sh
python3 -m http.server 8000   # 打开 http://localhost:8000
```

## 部署

本分支根目录就是站点根目录（`index.html` 在最外层）。发布方式取决于仓库
Settings → Pages 里配置的源分支：若指向 `web` 分支根目录，推上去即生效；
没有开 Pages 的话，把这个目录直接丢给任意静态托管也能用（无构建、无外部依赖）。
