# mc263.spec -- 把「单文件 ELF + 尾部 payload」封装成 rpm
# 安装后: /opt/mc263/mc263、/usr/bin/mc263、应用菜单项 mc263.desktop
#
# 关键点：单文件的数据追加在 ELF 尾部，rpm 默认的 brp-strip / debuginfo
# 流程会重写 ELF 并丢掉尾部字节，因此必须禁用 __os_install_post。
# 构建脚本会用 rpm2cpio 逐字节比对，确保 payload 完好。

# rpm 的 payload 压缩算法由 build-rpm.sh 按本机 rpm 能力传入
# （Fedora 45 / openSUSE TW: w19.zstdio；旧版 rpm 自动回退 w9.xzdio）
%global __os_install_post %{nil}
%global debug_package %{nil}
%define __strip /bin/true

# 若脚本未指定架构，默认 x86_64
%{!?payload_arch: %global payload_arch x86_64}
# 安装前缀（安装阶段由包管理器写入，不属于构建工具的输出文件）。
# 需要完全避开系统目录时：rpmbuild --define "mc263_prefix /your/project/path"
%{!?mc263_prefix: %global mc263_prefix /opt/mc263}
# 是否额外写 /usr（命令链接、桌面项、图标）。默认 0，避免向系统目录写文件；
# 需要应用菜单一键启动时显式 --define "mc263_system_links 1"。
%{!?mc263_system_links: %global mc263_system_links 0}

Name:           mc263
Version:        26.3
Release:        1%{?dist}
Summary:        Minecraft 26.3 single-file Linux launcher (self-extracting, bundled JRE)
# 本仓库源码采用 GPL-3.0-or-later（见仓库根目录 LICENSE）。
# 上游 zhanbalao/mc26-one 未声明许可证，且其唯一的脚本 build_262.py 已在本仓库移除；
# 本仓库现有代码为全新 C++23 实现，不含上游代码。
License:        GPL-3.0-or-later
URL:            https://github.com/zzxwithbus19/one-263-linux
BuildArch:      %{payload_arch}

Source0:        mc263-payload
Source1:        mc263.desktop
Source2:        mc263.svg
Source3:        LICENSE

# 运行时依赖（libzstd.so.1 / libstdc++.so.6 / libc）由 rpm 自动扫描 DT_NEEDED 生成，
# 无需手写；Fedora 与 openSUSE Tumbleweed 的库名差异也因此自动规避。
# 注意：本包不含 Mojang 的游戏文件，payload 由使用者用正版启动器获取的文件自行构建。

%description
Minecraft 26.3 的 Linux 单文件启动器：内置 Java 运行时、游戏库、资源与
Linux natives，首次运行自解压到用户缓存目录，随后一键启动游戏。
以 rpm 安装后可从应用菜单点击 "Minecraft 26.3"，或执行 mc263 命令。
本 RPM 由构建者自行生成，不包含任何 Mojang 版权素材。

%prep
# 无源码包，直接安装预构建的单文件

%build
# 无编译步骤

%install
rm -rf %{buildroot}
install -d -m 0755 %{buildroot}%{mc263_prefix}
install -m 0755 %{SOURCE0} %{buildroot}%{mc263_prefix}/mc263
install -m 0644 %{SOURCE1} %{buildroot}%{mc263_prefix}/mc263.desktop
install -m 0644 %{SOURCE2} %{buildroot}%{mc263_prefix}/mc263.svg
install -m 0644 %{SOURCE3} %{buildroot}%{mc263_prefix}/LICENSE
%if "%{mc263_system_links}" == "1"
install -d -m 0755 %{buildroot}%{_bindir}
ln -s %{mc263_prefix}/mc263 %{buildroot}%{_bindir}/mc263
install -d -m 0755 %{buildroot}%{_datadir}/applications
install -m 0644 %{SOURCE1} %{buildroot}%{_datadir}/applications/mc263.desktop
install -d -m 0755 %{buildroot}%{_datadir}/icons/hicolor/scalable/apps
install -m 0644 %{SOURCE2} %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/mc263.svg
%endif

%files
%license %{mc263_prefix}/LICENSE
%dir %{mc263_prefix}
%{mc263_prefix}/mc263
%{mc263_prefix}/mc263.desktop
%{mc263_prefix}/mc263.svg
%if "%{mc263_system_links}" == "1"
%{_bindir}/mc263
%{_datadir}/applications/mc263.desktop
%{_datadir}/icons/hicolor/scalable/apps/mc263.svg
%endif

%changelog
* Thu Nov 05 2026 mc263 build tooling
- C++23 rewrite of build_262.py for Linux, self-extracting single file
