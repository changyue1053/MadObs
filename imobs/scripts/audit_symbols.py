#!/usr/bin/env python3
"""共享库导出符号审计（实施计划 P6/架构 §18 符号隔离）。

三内核共存于同一 App：各自只准导出自己的 C ABI 前缀（imlk_/imdb_/imobs_），
其余符号一律 hidden——符号污染是共存链接事故的第一来源。
"""
import subprocess
import sys
import platform

ALLOWED_PREFIXES = ("_imobs_", "_madobs_") if platform.system() == "Darwin" else ("imobs_", "madobs_")
ALLOWLIST = {"__mh_dylib_header"}  # Mach-O 头符号（链接器生成，无害）


def main() -> int:
    lib = sys.argv[1]
    out = subprocess.run((["nm", "-gU", lib] if platform.system() == "Darwin" else ["nm", "-D", "--defined-only", lib]), capture_output=True, text=True, check=True).stdout
    bad = []
    total = 0
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        sym = parts[2]
        total += 1
        if sym in ALLOWLIST or any(sym.startswith(p) for p in ALLOWED_PREFIXES):
            continue
        bad.append(sym)
    if bad:
        print("非法导出符号（应为 hidden）：")
        for s in bad:
            print("  " + s)
        return 1
    print(f"符号审计通过：{total} 个导出符号全部合规（imobs_ 前缀）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
