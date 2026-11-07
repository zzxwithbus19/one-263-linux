# mc26-one -- C++23 构建（源码位于仓库根目录）
# 目标平台: Fedora 43+ / openSUSE Tumbleweed (GCC >= 15, libzstd, rpm >= 4.18)
#   make                 编译 launcher + mc263-build
#   make check           用伪造的 26.3 游戏目录跑端到端自测
#   make USE_ZSTD=0      关闭 libzstd，产出零第三方运行库依赖的 v1 单文件
#   make rpm             把 dist/mc263 打成 .rpm
#   make info            显示编译器/系统库配置

CXX      ?= g++
STD      := -std=c++23
OPT      := -O2
WARN     := -Wall -Wextra -Wpedantic
INCLUDES := -Iinclude
COMMON   := $(STD) $(OPT) $(WARN) $(INCLUDES)

BUILD    := build
DIST     := dist
LAUNCHER := $(BUILD)/launcher
BUILDER  := $(BUILD)/mc263-build
HEADERS  := $(wildcard include/*.hpp)

# ---- 系统库（pkg-config，Fedora / openSUSE TW 均自带）----
ZLIB_CFLAGS := $(shell pkg-config --cflags zlib 2>/dev/null)
ZLIB_LIBS   := $(shell pkg-config --libs zlib 2>/dev/null)
ifeq ($(ZLIB_LIBS),)
ZLIB_LIBS := -lz
endif

# Fedora 默认不装 libstdc++-static，故默认动态链接
#（目标平台 Fedora/TW 的 libstdc++ 均为 GCC 15+，C++23 二进制可直接运行；
#  rpmbuild 会自动生成 libstdc++.so.6 依赖）。装了静态库时可 STATIC_CXX=1。
STATIC_CXX ?= 0
ifeq ($(STATIC_CXX),1)
STATIC_FLAGS := -static-libstdc++ -static-libgcc
else
STATIC_FLAGS :=
endif

USE_ZSTD ?= 1
ZSTD_CFLAGS := $(shell pkg-config --cflags libzstd 2>/dev/null)
ZSTD_LIBS   := $(shell pkg-config --libs libzstd 2>/dev/null)
ifeq ($(USE_ZSTD),1)
ifeq ($(ZSTD_LIBS),)
$(warning 未找到 libzstd (libzstd-devel / libzstd-devel-32bit)，回退到 v1 STORED payload)
USE_ZSTD := 0
endif
endif

ifeq ($(USE_ZSTD),1)
LAUNCHER_DEFS := -DMC263_WITH_ZSTD
LAUNCHER_LIBS := $(ZSTD_LIBS)
BUILDER_DEFS  := -DMC263_WITH_ZLIB -DMC263_WITH_ZSTD
BUILDER_LIBS  := $(ZLIB_LIBS) $(ZSTD_LIBS)
PAYLOAD_FMT   := v2 (zstd 压缩, 需 libzstd)
else
LAUNCHER_DEFS :=
LAUNCHER_LIBS :=
BUILDER_DEFS  := -DMC263_WITH_ZLIB
BUILDER_LIBS  := $(ZLIB_LIBS)
PAYLOAD_FMT   := v1 (STORED zip, 零第三方运行库)
endif

CORE_TEST := $(BUILD)/core-test
QT_APP    := $(BUILD)/mc263-qt
USE_QT6   ?= 0
QT6_CFLAGS := $(shell pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core 2>/dev/null)
QT6_LIBS   := $(shell pkg-config --libs Qt6Widgets Qt6Gui Qt6Core 2>/dev/null)

ifeq ($(USE_QT6),1)
ifeq ($(QT6_LIBS),)
$(error 未找到 Qt6 开发包：sudo dnf install qt6-qtbase-devel（openSUSE TW: qt6-base-devel）)
endif
endif

.PHONY: all check rpm info clean help qt

all: $(LAUNCHER) $(BUILDER)
ifeq ($(USE_QT6),1)
all: $(QT_APP)
endif

$(LAUNCHER): src/launcher.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(COMMON) $(LAUNCHER_DEFS) $(ZSTD_CFLAGS) $(STATIC_FLAGS) \
	  $< -o $@ $(LAUNCHER_LIBS)

$(BUILDER): src/build_263.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(COMMON) $(BUILDER_DEFS) $(ZLIB_CFLAGS) $(ZSTD_CFLAGS) \
	  $< -o $@ $(BUILDER_LIBS)

# HMCL 式核心逻辑单测（不依赖 Qt / 不联网）
$(CORE_TEST): tests/core_test.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(COMMON) -pthread $< -o $@

# Qt6 原生启动器：无 Q_OBJECT / 无需 moc，单个 ELF
$(QT_APP): src/mc263_qt.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(COMMON) -pthread -fPIC $(QT6_CFLAGS) $< -o $@ $(QT6_LIBS)

check: all $(CORE_TEST)
	$(CORE_TEST)
	bash tests/selftest.sh --builder "$(abspath $(BUILDER))" --launcher "$(abspath $(LAUNCHER))" \
	     --format "$(PAYLOAD_FMT)"

qt:
	@$(MAKE) --no-print-directory USE_QT6=1 $(QT_APP)
	@printf '已构建 %s；桌面直接运行，或 QT_QPA_PLATFORM=offscreen %s 做冒烟测试\n' '$(QT_APP)' '$(QT_APP)'

rpm:
	bash packaging/build-rpm.sh --payload "$(abspath $(DIST)/mc263)"

info:
	@$(CXX) --version | head -1
	@printf '标准      : %s\n' '$(STD)'
	@printf 'payload   : %s\n' '$(PAYLOAD_FMT)'
	@printf 'zlib      : %s\n' '$(ZLIB_LIBS)'
	@printf 'libzstd   : %s\n' '$(ZSTD_LIBS)'
	@printf 'launcher  : %s\n' '$(LAUNCHER)'
	@printf 'builder   : %s\n' '$(BUILDER)'
	@printf 'qt app    : %s (make qt 构建)\n' '$(QT_APP)'
	@printf 'Qt6       : %s\n' '$(if $(QT6_LIBS),可用,缺 qt6-qtbase-devel)'
	@printf '沙箱根    : %s (MC263_SANDBOX_ROOT 可覆盖)\n' '$(abspath .)'

clean:
	rm -rf $(BUILD) $(DIST)

help:
	@printf '目标: all / check / qt / rpm / info / clean\n'
	@printf '  make qt           构建 Qt6 原生启动器 build/mc263-qt（单个 ELF）\n'
	@printf '  make USE_QT6=1    默认构建也包含 Qt 启动器\n'
	@printf '路径安全: 输出默认只写项目沙箱内；/、/home、/etc、/usr 等一律拒绝\n'
	@printf '          build/mc263-build --check-paths 可预检；MC263_SANDBOX_ROOT=<dir> 改沙箱根\n'
	@printf '变量: USE_ZSTD=0 关闭 zstd；STATIC_CXX=1 静态链接 libstdc++；CXX=clang++ 换编译器\n'
