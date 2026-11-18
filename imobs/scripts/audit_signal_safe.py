#!/usr/bin/env python3
"""signal-safe 审计（W6 残项 4；架构 §10 崩溃采集三件事 / §18 crash 审计）。

崩溃 handler 只准做三件事：向预分配 mmap 崩溃区纯内存写（含栈回溯）、fsync、
re-raise。清单外函数（malloc/printf/new/锁/任何 C++ 异常机制）出现即红灯。
此前白名单靠 crash_sentry.cc 注释约定，本脚本将其自动化为 CI 红灯。

方案：源码调用面解析（而非 nm/objdump 符号扫描）。理由：
  1. handler 是单 TU 内的小函数（无模板、无宏展开混淆；darwin 寄存器宏在源码
     可见），源码调用面即真实调用面，判定确定且跨平台零工具链依赖；
  2. nm 只有 TU 级未定义符号，无法把 handler 调用链与同文件冷路径分开——
     Install/MapFile 合法使用 open/mmap/ftruncate/std::string，TU 级扫描必然
     误伤或为避免误伤而漏检；
  3. objdump 反汇编按符号切片可精确归因，但输出格式跨 llvm/binutils、跨
     arch（bl/callq）矩阵脆弱，CI 可移植性差。
  残余风险（接受并写明）：编译器物化调用（memcpy 下沉、stack protector）源码
  面不可见——以「handler 调用链内禁堆类型/异常关键字」的文本禁令兜底；发版
  前可用 objdump 对 OnFatalSignal 符号人工复核一次（上游属主动作）。

审计逻辑：
  1. 从安装语句 `sa.sa_sigaction = &<root>;` 自动锚定 handler 根（锚点消失即
     红灯——改名/换安装方式必须同步审计）；
  2. 文件内函数定义建图，从根做调用闭包（当前应为 OnFatalSignal → CrashRegs /
     NextFrame；MapFile 不可达，属 Install 冷路径——闭包正确性的天然对照）；
  3. 闭包内每个调用 token 必须落在白名单 ∪ 闭包内函数 ∪ C++ 关键字/转型；
  4. 纵深：闭包体内出现堆类型/new/异常关键字等文本也红灯（防非调用形态引入）。
  本审计不设 audit-allow 豁免——handler 内没有"冷路径"概念，放宽只能改
  白名单本身（上游属主决策，禁止为过审自行放宽）。

白名单口径（§10）：write/pwrite/fsync（minidump 落盘族）+ signal/raise/kill
（re-raise 族）+ time（POSIX async-signal-safe 清单成员，crash-loop 环墙钟）。
mmap 页写是纯内存写非函数调用，无需白名单条目。
"""
import pathlib
import re
import sys

TARGET = "src/platform/crash_sentry.cc"

# 外部调用白名单（POSIX async-signal-safe 子集，按 §10 三件事裁剪）
WHITELIST = {
    "write", "pwrite", "fsync",          # minidump/marker 落盘族（当前实现走 mmap 页写，保留给未来）
    "signal", "raise", "kill",           # 恢复默认处置 + re-raise 族
    "time",                              # POSIX ASS 清单成员：crash-loop 环的墙钟
}

# 平台寄存器访问宏：纯字段读，无调用语义（darwin ucontext 取 pc/lr/fp）
REG_MACROS = {
    "__darwin_arm_thread_state64_get_pc",
    "__darwin_arm_thread_state64_get_lr",
    "__darwin_arm_thread_state64_get_fp",
}

# 语言构造：不是函数调用
KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "alignof", "decltype",
    "static_cast", "reinterpret_cast", "const_cast", "dynamic_cast",
    "static_assert", "defined", "catch",
}

# 纵深文本禁令：即使不以调用形态出现也红灯（handler 调用链内绝不该有）
FORBIDDEN_TEXT = [
    (r"std::string\b", "堆类型"),
    (r"std::vector\b", "堆类型"),
    (r"std::map\b|unordered_map", "堆类型"),
    (r"\bnew\b|\bmake_unique\b|\bmake_shared\b", "堆分配"),
    (r"\bmalloc\s*\(|\bcalloc\s*\(|\brealloc\s*\(|\bfree\s*\(", "堆分配"),
    (r"\bthrow\b|\btry\b", "异常机制"),
    (r"std::mutex|\block_guard\b|\bunique_lock\b", "锁"),
]

CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
DEF_RE = re.compile(
    r"(?:^|\n)[ \t]*(?:[\w:<>\*&~\[\]]+[\s\*&]+)+(?:(?:\w+)::)?(\w+)\s*\(([^;{}]*)\)\s*(?:const)?\s*{")


def strip_comments(text: str) -> str:
    """剥离 // 与 /* */ 注释（保留换行以维持行号）。"""
    out = []
    i = 0
    n = len(text)
    in_line = False
    in_block = False
    while i < n:
        c = text[i]
        if in_line:
            if c == "\n":
                in_line = False
                out.append(c)
            i += 1
            continue
        if in_block:
            if text.startswith("*/", i):
                in_block = False
                i += 2
            else:
                if c == "\n":
                    out.append(c)
                i += 1
            continue
        if text.startswith("//", i):
            in_line = True
            i += 2
            continue
        if text.startswith("/*", i):
            in_block = True
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def find_functions(code: str):
    """文件内函数定义 → {name: (body_text, start_line)}（花括号配平取函数体）。"""
    funcs = {}
    for m in DEF_RE.finditer(code):
        name = m.group(1)
        if name in KEYWORDS:
            continue
        brace = code.find("{", m.end() - 1)
        depth = 0
        j = brace
        while j < len(code):
            if code[j] == "{":
                depth += 1
            elif code[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = code[brace:j + 1]
        funcs[name] = (body, code.count("\n", 0, brace) + 1)
    return funcs


def calls_in(body: str):
    return {c for c in CALL_RE.findall(body)}


def main() -> int:
    root_dir = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    path = root_dir / TARGET
    if not path.exists():
        print(f"signal-safe 审计失败：{TARGET} 缺失（审计目标需与代码同步维护）")
        return 1
    code = strip_comments(path.read_text(encoding="utf-8"))

    roots = re.findall(r"sa_sigaction\s*=\s*&?\s*(\w+)\s*;", code)
    if not roots:
        print("signal-safe 审计失败：未找到 handler 安装语句（sa_sigaction = &...）——"
              "锚点消失，改名/换安装方式必须同步本审计")
        return 1

    funcs = find_functions(code)
    missing = [r for r in roots if r not in funcs]
    if missing:
        print(f"signal-safe 审计失败：handler 根 {missing} 在 {TARGET} 内无定义"
              "（跨 TU handler 不在审计契约内，需先收敛回本 TU）")
        return 1

    # 调用闭包
    closure = []
    pending = list(dict.fromkeys(roots))
    while pending:
        fn = pending.pop(0)
        if fn in closure:
            continue
        closure.append(fn)
        for callee in calls_in(funcs[fn][0]):
            if callee in funcs and callee not in closure:
                pending.append(callee)

    hits = []
    used_external = set()
    for fn in closure:
        body, start_line = funcs[fn]
        # 1) 调用面白名单
        for callee in sorted(calls_in(body)):
            if callee in KEYWORDS or callee in funcs or callee in REG_MACROS:
                continue
            if callee in WHITELIST:
                used_external.add(callee)
                continue
            for off, line in enumerate(body.split("\n")):
                if re.search(rf"\b{re.escape(callee)}\s*\(", line):
                    hits.append(f"{TARGET}:{start_line + off}: [清单外调用] {fn} → "
                                f"{callee}()：{line.strip()}")
        # 2) 纵深文本禁令
        for off, line in enumerate(body.split("\n")):
            for pat, why in FORBIDDEN_TEXT:
                if re.search(pat, line):
                    hits.append(f"{TARGET}:{start_line + off}: [{why}] {fn} 体内：{line.strip()}")

    if hits:
        print("signal-safe 审计失败（handler 调用链出现白名单外构造）：")
        for h in sorted(set(hits)):
            print("  " + h)
        return 1

    print(f"signal-safe 审计通过：root={'/'.join(roots)}；调用链 {len(closure)} 函数"
          f"（{', '.join(closure)}）；外部调用面 {{{', '.join(sorted(used_external))}}} ⊆ 白名单；"
          f"0 违规")
    return 0


if __name__ == "__main__":
    sys.exit(main())
