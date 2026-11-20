#!/usr/bin/env python3
"""生成 compile_commands.json（给 VS Code C/C++ 扩展 / clangd 用）。

Qt 的头文件路径在不同发行版不一样：
  Ubuntu/Debian  /usr/include/x86_64-linux-gnu/qt6
  Fedora/Arch    /usr/include/qt6
所以不要手写或提交带绝对路径的 compile_commands.json，构建前跑一次：
  make compile-db
"""
import json
import os
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def pkg_config_flags(pkg):
    try:
        out = subprocess.run(["pkg-config", "--cflags", pkg],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        sys.exit(f"错误: pkg-config 找不到 {pkg}（Ubuntu: sudo apt install qt6-base-dev）")
    return shlex.split(out)


def main():
    cxx = os.environ.get("CXX", "clang++")
    qt = pkg_config_flags("Qt6Widgets")
    common = ["-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Iinclude"]

    entries = [
        {
            "file": os.path.join(ROOT, "src/mc263_qt.cpp"),
            "arguments": [cxx, *common, "-rdynamic", "-pthread", "-fPIC",
                          *qt, os.path.join(ROOT, "src/mc263_qt.cpp"),
                          "-o", os.path.join(ROOT, "build/mc263-qt")],
            "directory": ROOT,
            "output": os.path.join(ROOT, "build/mc263-qt"),
        },
        {
            "file": os.path.join(ROOT, "tests/core_test.cpp"),
            "arguments": [cxx, *common, "-pthread",
                          os.path.join(ROOT, "tests/core_test.cpp"),
                          "-o", os.path.join(ROOT, "build/core-test")],
            "directory": ROOT,
            "output": os.path.join(ROOT, "build/core-test"),
        },
    ]

    path = os.path.join(ROOT, "compile_commands.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(entries, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"已写入 {path}")
    for e in entries:
        print(f"  {os.path.relpath(e['file'], ROOT)}")


if __name__ == "__main__":
    main()
