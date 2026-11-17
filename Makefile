# mc26-one -- Qt6 原生 Minecraft 启动器（C++23 + Qt6 Widgets）
# 目标平台: Fedora 43+ / openSUSE Tumbleweed（GCC >= 15）
#   make           编译 build/mc263-qt
#   make check     核心单测 + C++ 下载器自测（全部离线）
#   make rpm       调 packaging/build-rpm.sh 打 rpm
#   make arch      调 packaging/build-arch.sh 打 pacman 包（PKGBUILD，不走 AUR）
#   make deb       调 packaging/build-deb.sh 打 .deb（Ubuntu 26.04+ / Debian 13+）
#   make info      显示编译器 / Qt6 状态

#   make compile-db 重新生成 compile_commands.json（换发行版/机器后跑一次）

CXX      ?= g++
STD      := -std=c++23
OPT      := -O2
WARN     := -Wall -Wextra -Wpedantic
INCLUDES := -Iinclude
COMMON   := $(STD) $(OPT) $(WARN) $(INCLUDES)

BUILD     := build
DIST      := dist
QT_APP        := $(BUILD)/mc263-qt
CORE_TEST     := $(BUILD)/core-test
DOWNLOAD_TEST := $(BUILD)/downloader-test
HEADERS       := $(wildcard include/*.hpp)

QT6_CFLAGS := $(shell pkg-config --cflags Qt6Widgets 2>/dev/null)
QT6_LIBS   := $(shell pkg-config --libs Qt6Widgets 2>/dev/null)

ifeq ($(QT6_LIBS),)
$(warning 未找到 Qt6 开发包；Fedora: sudo dnf install qt6-qtbase-devel / openSUSE: sudo zypper install qt6-qtbase-devel)
endif

.PHONY: all check qt rpm arch deb compile-db info clean help

all: $(QT_APP)

# Qt6 启动器：无 Q_OBJECT / 无需 moc，单个 ELF
$(QT_APP): src/mc263_qt.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(COMMON) -rdynamic -pthread -fPIC $(QT6_CFLAGS) $< -o $@ $(QT6_LIBS)

# 核心逻辑离线单测（不依赖 Qt）
$(CORE_TEST): tests/core_test.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(COMMON) -pthread $< -o $@

# 下载核心测试 CLI（离线 mock CDN；不依赖 Qt）
$(DOWNLOAD_TEST): tests/downloader_test.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(COMMON) -pthread $< -o $@

qt: all
	@printf '已构建 %s；直接运行，或 QT_QPA_PLATFORM=offscreen %s 做冒烟测试\n' '$(QT_APP)' '$(QT_APP)'

check: $(CORE_TEST) $(DOWNLOAD_TEST)
	$(CORE_TEST)
	bash tests/downloader_test.sh

rpm: all
	bash packaging/build-rpm.sh

arch:
	bash packaging/build-arch.sh $(ARCH_ARGS)

deb: all
	bash packaging/build-deb.sh

compile-db:
	@CXX="$(CXX)" python3 tools/gen_compile_db.py

info:
	@$(CXX) --version | head -1
	@printf '标准      : %s\n' '$(STD)'
	@printf 'Qt6       : %s\n' '$(if $(QT6_LIBS),可用,缺 qt6-qtbase-devel)'
	@printf 'qt app    : %s\n' '$(QT_APP)'
	@printf '沙箱根    : %s (MC263_SANDBOX_ROOT 可覆盖)\n' '$(abspath .)'

clean:
	rm -rf $(BUILD) $(DIST)

help:
	@printf '目标: all / check / rpm / arch / deb / compile-db / info / clean\n'
	@printf '  make              编译 Qt6 启动器 build/mc263-qt\n'
	@printf '  make check        核心单测 + C++ 下载器（mock CDN）自测\n'
	@printf '  make rpm          调 packaging/build-rpm.sh 打 rpm\n'
	@printf '  make arch [ARCH_ARGS=--nodeps]  打 pacman 包（源码构建，不走 AUR）\n'
	@printf '  make deb          打 .deb（Ubuntu 26.04+ / Debian 13+）\n'
	@printf '  make compile-db   重新生成 compile_commands.json（VSCode/clangd 用）\n'
	@printf '  make clean        删除 build/ 与 dist/\n'
	@printf '变量: CXX=clang++ 换编译器；MC263_SANDBOX_ROOT=<dir> 改沙箱根\n'
