# mc263-source.spec -- 从源码构建的 rpm（给 COPR / OBS / 自建仓库用）
#
# 和 packaging/mc263.spec 的区别：
#   mc263.spec        打包预编译二进制（本地快速出包，用户不需要编译环境）
#   mc263-source.spec 在 mock/chroot 里现编源码（COPR、OBS、官方构建都用这个）
#
# 打包：
#   bash packaging/make-source.sh          # 生成 dist/mc263-26.3.tar.gz
#   copr-cli build mc263 dist/mc263-26.3.tar.gz

%global mc263_version 26.3
%global debug_package %{nil}

Name:           mc263
Version:        %{mc263_version}
Release:        2%{?dist}
Summary:        Minecraft 26.3 launcher (Qt6 native, Wayland-ready)
License:        GPL-3.0-or-later
URL:            https://github.com/zzxwithbus19/one-263-linux
# 必须是具体架构：默认 noarch 会让 mock 构建失败（产物是 x86_64 ELF）
BuildArch:      %{?_arch}
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  python3
%if 0%{?fedora}
BuildRequires:  pkgconfig(Qt6Widgets)
BuildRequires:  qt6-qtbase-devel
%endif
%if 0%{?suse_version}
# openSUSE 的 Qt6 开发包叫 qt6-base-devel，且没有 pkgconfig(Qt6Widgets) 这个 provide
BuildRequires:  qt6-base-devel
BuildRequires:  cmake(Qt6Widgets)
%endif

# 运行时依赖：Qt6 库由 rpm 从 ELF 的 DT_NEEDED 自动生成，这里写死也行
Requires:       python3
%if 0%{?fedora}
Recommends:     qt6-qtwayland
%endif
%if 0%{?suse_version}
Recommends:     libQt6WaylandClient6
%endif

%description
Minecraft 26.3 的 Linux 原生启动器（Qt6 Widgets 手写界面，不用 moc、不套 Electron）。

单个 ELF + 用户目录下的 minecraft/ 与 .mc263/：
- 下载游戏：Mojang 官方 CDN 或 bmclapi 镜像，python3 多线程 + SHA1 校验 + 断点续传
- Java 探测与版本闸门：后台扫描系统 Java 与 Mojang 运行时，不达标时拦住启动并给出下载入口
- 兼容性查验：架构、Java 版本、依赖库、natives、图形音频库、内存与磁盘逐项检查
- Mod 与加载器：搜索 Modrinth（可排序/过滤），一键安装 Fabric / Quilt 加载器
- 版本隔离：每个版本的存档与 Mod 独立存放，互不干扰
- 内存默认交给 JVM 管理；游戏语言跟随系统 locale（识别不到用 en_us）
- 显示后端：显式 QT_QPA_PLATFORM 优先，否则按会话选 Wayland / XWayland

本包不包含任何 Mojang 游戏文件或 Java 运行时，也不提供账号认证；
首次使用请在「下载游戏」页获取。

%prep
%autosetup -n %{name}-%{version}

%build
make %{?_smp_mflags}

%install
install -D -m 0755 build/mc263-qt              %{buildroot}%{_bindir}/mc263-qt
install -D -m 0644 packaging/mc263.desktop     %{buildroot}%{_datadir}/applications/mc263.desktop
install -D -m 0644 packaging/mc263.svg         %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/mc263.svg
install -D -m 0644 LICENSE                     %{buildroot}%{_datadir}/licenses/%{name}/LICENSE
install -D -m 0644 packaging/mc263.metainfo.xml %{buildroot}%{_datadir}/metainfo/mc263.metainfo.xml

%check
# 核心单测离线可跑；下载器自测需要 mock CDN，放在 %check 里也安全
make check

%files
%{_bindir}/mc263-qt
%{_datadir}/applications/mc263.desktop
%{_datadir}/icons/hicolor/scalable/apps/mc263.svg
%license %{_datadir}/licenses/%{name}/LICENSE
%{_datadir}/metainfo/mc263.metainfo.xml

%changelog
* Sat Nov 07 2026 mc263 <mc263@users.noreply.github.com> - %{mc263_version}-1
- Qt6 native launcher (Wayland-first), built from source
