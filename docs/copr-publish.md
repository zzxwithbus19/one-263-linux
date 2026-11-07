# 发布到 COPR（Fedora）· 操作清单

> 项目地址：https://copr.fedorainfracloud.org/coprs/cxc123/one-263-linux/
> 项目 ID 270168 · 已建仓库：Fedora 45 (x86_64)、openSUSE Tumbleweed (x86_64)
> 当前状态：**还没有任何构建**（Last Build: No builds），描述还是旧版文案。

---

## 1. 先改项目描述（Overview 页）

COPPR 项目页的 **Description** 与 **Installation Instructions** 两个框现在是旧文案（还在讲
「上游 zhanbalao/mc26-one 未声明许可、build_262.py 已移除」那段历史），与现在的代码不符，
建议直接替换成下面两段（与 rpm 包里的 `%description` 保持一致）。

**Description**

```
Minecraft 26.3 的 Linux 原生启动器（Qt6 Widgets 手写界面，不用 moc、不套 Electron）。

单个 ELF + 用户目录下的 minecraft/ 与 .mc263/：
- 下载游戏：Mojang 官方 CDN 或 bmclapi 镜像，多线程 + SHA1 校验 + 断点续传
- Java 探测与版本闸门：不达标时拦住启动并给出下载入口
- 兼容性查验：架构、Java 版本、依赖库、natives、图形音频库、内存与磁盘
- Mod 与加载器：搜索 Modrinth，一键安装 Fabric / Quilt 加载器
- 版本隔离：每个版本的存档与 Mod 独立存放
- 内存默认交给 JVM 管理；游戏语言跟随系统 locale
- 显示后端：显式 QT_QPA_PLATFORM 优先，否则按会话选 Wayland / XWayland

不包含任何 Mojang 游戏文件或 Java 运行时，也不提供账号认证。
```

**Installation Instructions**

```
sudo dnf install 'dnf-command(copr)'
sudo dnf copr enable cxc123/one-263-linux
sudo dnf install mc263

# 也可以不装插件，手写 repo 文件：
#   sudo tee /etc/yum.repos.d/mc263.repo <<'EOF'
#   [copr:copr.fedorainfracloud.org:cxc123:one-263-linux]
#   name=COPR:copr.fedorainfracloud.org:cxc123:one-263-linux
#   baseurl=https://copr.fedorainfracloud.org/coprs/cxc123/one-263-linux/fedora-45/
#   enabled=1
#   gpgcheck=1
#   gpgkey=https://copr.fedorainfracloud.org/keys/fedora-45-COPR.gpg
#   EOF
```

> 页面右侧 Quick Enable 给的是不带 release 的旧式命令
> （`dnf copr enable cxc123/one-263-linux`），**带 release 段的新写法更稳**，
> 不会因为仓库多而选错。

---

## 2. 上传第一次构建

我们这边已经准备好「从源码构建」用的包（COPR 在 mock 里现编，不能塞预编译二进制）：

```bash
# 注意：一定要在仓库目录里执行（或用绝对路径），否则会报「没有那个文件或目录」
cd /home/bus/mc26-one
bash packaging/make-source.sh          # → dist/mc263-26.3.tar.gz（并打印 sha256）

# 方式 A：copr-cli（推荐）
sudo dnf install copr-cli
# 先把配置文件铺好（copr-cli 2.7 不会自己建文件，缺了会报
# “File ~/.config/copr not found. Can not update.”）：
mkdir -p ~/.config
cat > ~/.config/copr <<EOF
[copr-cli]                      # 段名必须是 copr-cli，写成 [copr] 会报 KeyError: copr-cli
login = cxc123
token =
copr_url = https://copr.fedorainfracloud.org
EOF
chmod 600 ~/.config/copr
# token 要去网页生成：https://copr.fedorainfracloud.org/api/ → Create new token → 复制
# 填进上面配置的 token = 行；然后：
copr-cli whoami               # 确认身份（应输出 cxc123）
copr-cli whoami               # 确认身份
bash packaging/make-srcrpm.sh          # → dist/mc263-26.3-1.fc45.src.rpm
copr-cli build cxc123/one-263-linux dist/mc263-26.3-1.fc45.src.rpm

# 方式 B：网页
# Projects → 你的项目 → New build → 上传 tarball / src.rpm
```

> 如果 `copr-cli` 报 `Error: Response is not in JSON format`，多半是**上传的文件不存在**
> （命令跑在了别的目录）或 COPR 正在迁移 Pulp 时接口临时抽风：
> 先 `ls -l dist/mc263-26.3.tar.gz` 确认文件在，再用 `copr-cli --debug build …` 看原始响应，
> 还不行就直接走网页上传。

**必须传 .src.rpm，不能直接传 tarball**：COPR 的 v3 API 只收 `.src.rpm` / `.nosrc.rpm` / `.spec`，
直接传 tarball 会报 `You can upload only .src.rpm, .nosrc.rpm, and .spec files`。
`packaging/make-srcrpm.sh` 会先打 tarball，再用 `rpmbuild -bs` 把它和 spec 一起装进 src.rpm。

src.rpm 里的内容（COPR 就按这个在 mock 里构建）：

```
mc263-26.3-1.fc45.src.rpm
├── mc263.spec            ← 打包脚本会把 mc263-source.spec 复制成这个名字（与包名一致）
└── mc263-26.3.tar.gz    ← 源码树（顶层目录 mc263-26.3/）
```

源码 tarball 的结构（供参考）：

```
mc263-26.3.tar.gz
├── mc263.spec          ← 必须是「从源码构建」那份（packaging/mc263-source.spec）
└── mc263-26.3/         ← 顶层目录，名字必须与 pkgname-version 一致
    ├── Makefile  src/  include/  tests/  packaging/  LICENSE
```

要点（都已经在 `packaging/mc263-source.spec` 里处理好）：

- `BuildArch: %{?_arch}` —— **不加会被当成 noarch，构建必失败**
- `BuildRequires: gcc-c++ make pkgconfig(Qt6Widgets) qt6-qtbase-devel python3`
- `%prep` 用 `%autosetup -n mc263-26.3`，`%build` 跑 `make`，`%check` 跑 `make check`（全离线，mock 里能过）
- AppStream 元数据已随包装好（`packaging/mc263.metainfo.xml` → `/usr/share/metainfo/`）

---

## 3. 构建成功后怎么验证

```bash
sudo dnf install mc263
rpm -q mc263
mc263-qt                                # 图形界面
QT_QPA_PLATFORM=offscreen timeout 3 mc263-qt; echo $?   # 冒烟：期望 124
sudo dnf upgrade mc263                  # 有新版本时直接升级
```

签名方面：COPR 构建产物由 Fedora 官方密钥签名，用户 `dnf install` 时 rpm 会自动验签，
**不需要** `--nogpgcheck`，也不需要手动 import 公钥（dnf copr 插件会处理）。

---

## 4. openSUSE Tumbleweed 那个仓库

项目里同时建了 openSUSE Tumbleweed 仓库，但**第一轮只建议发布 Fedora**：

- SUSE 侧的 BuildRequires / Recommends 命名与 Fedora 不同（`libQt6WaylandClient6`），
  mock 环境也不一样，容易连续失败
- 处理方式：先只上传 Fedora 构建；openSUSE 那边在 Settings 里把该发行版标成
  **未发布 / unpublished**，或在确认能构建后再单独提交

---

## 5. 常见失败与对应修法

| 构建日志里的报错 | 原因 | 怎么改 |
|---|---|---|
| `noarch` 相关 / 架构不匹配 | spec 缺 `BuildArch` | 加 `BuildArch: %{?_arch}`（已加） |
| `You can upload only .src.rpm, .nosrc.rpm, and .spec files` | 直接传了 tarball | 先 `bash packaging/make-srcrpm.sh` 再传 `.src.rpm` |
| `Source0: mc263-26.3.tar.gz: not found` 或 “not like a spec file” | tarball 里没有顶层 spec，或目录名和版本对不上 | spec 放**包根**、目录名 `mc263-26.3`（`make-source.sh` 已保证） |
| `pkgconfig(Qt6Widgets): No such file` | 缺 BuildRequires | 加 `pkgconfig(Qt6Widgets)` + `qt6-qtbase-devel`（已加） |
| `%check` 阶段失败 | 单测需要额外工具 | 依赖已含 `python3`；若仍失败可临时把 `%check` 段删掉重发 |
| fedora-review 报 `non-standard email address` | changelog 邮箱格式 | 已改成 `mc263@users.noreply.github.com` |
| fedora-review 报 `no-manual-page` / `empty-%description` 之类 | 规范检查 | 按提示改 spec 后重新 `copr-cli build` |

### token 泄露 / 想换一枚

token 等同于账号密码，别贴到公开地方。已经配置好 login+token 之后，
`copr-cli new-api-token` 就能用了：它调 API 换发一枚新 token 并自动写回 `~/.config/copr`，
旧 token 立刻失效（去 https://copr.fedorainfracloud.org/api/ 也能手动吊销）。

### 命令行报 `Error: Response is not in JSON format`

这句话的意思是「服务端返回的不是 JSON」，常见三个原因，按顺序排查：

1. **没有 API token**（最常见）：Fedora 45 的 copr-cli 2.7 **没有 `login` 子命令**，认证靠 token：
   **token 必须去网页生成**：https://copr.fedorainfracloud.org/api/ → “Your API tokens” →
   Create new token → 复制，手工填进 `~/.config/copr` 的 `token =` 行。
   `copr-cli new-api-token` 是**轮换**已有 token 用的（源码里它要求配置里已经有 login+token
   才会去调 API 换发新的），当首次登录用会报
   “Current …/copr doesn not specify login/token. Can not update.”。
   另外：copr-cli 2.7 **不会自动创建配置文件**（缺了报 `File ~/.config/copr not found.`）；
   段名必须是 `[copr-cli]`，写成 `[copr]` 会报 `KeyError: copr-cli`
   （源码 copr/v3/helpers.py 读的是 raw_config["copr-cli"]）；字段 username/login/token/copr_url/gssapi；
   也可手工填 `token = …` 后用 `copr-cli whoami` 验证身份再 `copr-cli build`。
2. **被 Anubis 反爬拦了**：COPR 站点挂了 Anubis 的工作量证明，浏览器能过，纯 API 客户端可能被挡。
   copr-cli 的 API 基址是 `copr_url + /api_3/`（源码 copr/v3/proxies/__init__.py），
   与网页同域，所以**即使 token 配好也可能被拦** —— 症状相同：返回 HTML 挑战页而不是 JSON。
   5 秒诊断：`curl -sS https://copr.fedorainfracloud.org/api_3/ | head -c 200`
   —— 出 JSON 说明没被挡（那就纯粹是认证问题）；出 HTML 说明被挡，改走网页 New build 上传。
   用 `copr-cli --debug build …` 看原始响应里是不是 “Making sure you're not a bot”。
   遇到这种就**改用网页上传**（项目页 → New build），浏览器会跑那段 JS。
3. **Pulp 迁移期间接口抽风**：站点顶部有 “Ongoing data migration to Pulp” 横幅，过几分钟重试。

改完 spec 后重新打包再传：

```bash
bash packaging/make-source.sh
bash packaging/make-srcrpm.sh          # → dist/mc263-26.3-1.fc45.src.rpm
copr-cli build cxc123/one-263-linux dist/mc263-26.3-1.fc45.src.rpm
```
