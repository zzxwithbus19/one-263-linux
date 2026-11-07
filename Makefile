# mc26-one -- Qt6 原生 Minecraft 启动器（C++23 + Qt6 Widgets）
# 目标平台: Fedora 43+ / openSUSE Tumbleweed（GCC >= 15）
#   make           编译 build/mc263-qt
#   make check     核心单测 + 内嵌下载器自测（全部离线）
#   make rpm       调 packaging/build-rpm.sh 打 rpm
#   make info      显示编译器 / Qt6 状态

CXX      ?= g++
STD      := -std=c++23
OPT      := -O2
WARN     := -Wall -Wextra -Wpedantic
INCLUDES := -Iinclude
COMMON   := $(STD) $(OPT) $(WARN) $(INCLUDES)

BUILD     := build
DIST      := dist
QT_APP    := $(BUILD)/mc263-qt
CORE_TEST := $(BUILD)/core-test
HEADERS   := $(wildcard include/*.hpp)

QT6_CFLAGS := $(shell pkg-config --cflags Qt6Widgets 2>/dev/null)
QT6_LIBS   := $(shell pkg-config --libs Qt6Widgets 2>/dev/null)

ifeq ($(QT6_LIBS),)
$(warning 未找到 Qt6 开发包；Fedora: sudo dnf install qt6-qtbase-devel / openSUSE: sudo zypper install qt6-qtbase-devel)
endif

.PHONY: all check qt rpm info clean help

all: $(QT_APP)

# Qt6 启动器：无 Q_OBJECT / 无需 moc，单个 ELF
$(QT_APP): src/mc263_qt.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(COMMON) -pthread -fPIC $(QT6_CFLAGS) $< -o $@ $(QT6_LIBS)

# 核心逻辑离线单测（不依赖 Qt）
$(CORE_TEST): tests/core_test.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(COMMON) -pthread $< -o $@

qt: all
	@printf '已构建 %s；直接运行，或 QT_QPA_PLATFORM=offscreen %s 做冒烟测试\n' '$(QT_APP)' '$(QT_APP)'

check: $(CORE_TEST)
	$(CORE_TEST)
	bash tests/downloader_test.sh

rpm: all
	bash packaging/build-rpm.sh

info:
	@$(CXX) --version | head -1
	@printf '标准      : %s\n' '$(STD)'
	@printf 'Qt6       : %s\n' '$(if $(QT6_LIBS),可用,缺 qt6-qtbase-devel)'
	@printf 'qt app    : %s\n' '$(QT_APP)'
	@printf '沙箱根    : %s (MC263_SANDBOX_ROOT 可覆盖)\n' '$(abspath .)'

clean:
	rm -rf $(BUILD) $(DIST)

help:
	@printf '目标: all / check / rpm / info / clean\n'
	@printf '  make              编译 Qt6 启动器 build/mc263-qt\n'
	@printf '  make check        核心单测 + 内嵌下载器（mock CDN）自测\n'
	@printf '  make rpm          调 packaging/build-rpm.sh 打 rpm\n'
	@printf '  make clean        删除 build/ 与 dist/\n'
	@printf '变量: CXX=clang++ 换编译器；MC263_SANDBOX_ROOT=<dir> 改沙箱根\n'
