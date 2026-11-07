# mc263.spec -- 预编译二进制的 rpm 封装（装包时不需要编译器 / Qt 开发包）
#
# 由 packaging/build-rpm.sh 调用：先把 build/mc263-qt 放进 SOURCES，再 rpmbuild -bb。
# 手工做法：
#   mkdir -p build/rpmbuild/SOURCES
#   cp build/mc263-qt packaging/mc263.desktop packaging/mc263.svg LICENSE build/rpmbuild/SOURCES/
#   rpmbuild -bb packaging/mc263.spec --define "_topdir $PWD/build/rpmbuild" \
#            --define "mc263_version 26.3" --define "payload_arch $(uname -m)"
#
# 安装后：/usr/bin/mc263-qt + 应用菜单「Minecraft 26.3」。
# 配置不进系统目录：/usr/bin 不可写时启动器自动改用 ~/.local/share/mc263/。

%{!?mc263_version: %global mc263_version 26.3}
%{!?mc263_release: %global mc263_release 1}
%{!?payload_arch: %global payload_arch x86_64}
# 不生成 -debuginfo 子包：省掉 debugedit / eu-strip 依赖
%global debug_package %{nil}
%global __strip /bin/true

Name:           mc263
Version:        %{mc263_version}
Release:        %{mc263_release}%{?dist}
Summary:        Minecraft 26.3 launcher (Qt6 native, Wayland-ready)
License:        GPL-3.0-or-later
URL:            https://github.com/zzxwithbus19/one-263-linux
BuildArch:      %{payload_arch}

Source0:        mc263-qt
Source1:        mc263.desktop
Source2:        mc263.svg
Source3:        LICENSE
Source4:        mc263.metainfo.xml

# Qt6 的库依赖由 rpm 自动从 ELF 的 DT_NEEDED 生成（libQt6Widgets.so.6 等），
# Fedora / openSUSE 都能解析，不用写死发行版的包名。
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
# 预编译二进制，无需解包源码

%build
# 无编译步骤

%install
rm -rf %{buildroot}
install -D -m 0755 %{SOURCE0}              %{buildroot}%{_bindir}/mc263-qt
install -D -m 0644 %{SOURCE1}              %{buildroot}%{_datadir}/applications/mc263.desktop
install -D -m 0644 %{SOURCE2}              %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/mc263.svg
install -D -m 0644 %{SOURCE3}              %{buildroot}%{_datadir}/licenses/%{name}/LICENSE
install -D -m 0644 %{SOURCE4}              %{buildroot}%{_datadir}/metainfo/mc263.metainfo.xml

%files
%{_bindir}/mc263-qt
%{_datadir}/applications/mc263.desktop
%{_datadir}/icons/hicolor/scalable/apps/mc263.svg
%license %{_datadir}/licenses/%{name}/LICENSE
%{_datadir}/metainfo/mc263.metainfo.xml

%changelog
* Sat Nov 07 2026 mc263 <noreply@example.com> - 26.3-1
- Qt6 native launcher (Wayland-first), prebuilt binary package
