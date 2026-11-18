#!/usr/bin/env python3
"""热路径防腐审计（实施计划 P0.3；W6 残项 3 对齐架构 §18 字面）。

打点发生在 UI 线程 / IMLink I/O 线程 / IMDB 写线程上，热路径代码里出现锁、
堆分配或系统调用，就是把观测成本转嫁成宿主卡顿。此脚本在 CI 里强制：
命中禁用符号即红灯。

覆盖面（§18 字面：`api/` 与 `core/signal/` 目录禁 new/malloc/mutex/系统调用）：
  - src/api/、src/core/signal/ 两目录递归全量（.h/.cc）；
  - 存量热路径头 include/imobs/event.h、event_ring.h（TryPush/PopBatch 的
    inline 实现在头文件里，保留防倒退）。
  include/imobs/ 其余模块头是对外 API 面，不在 §18 审计区字面内——其含 inline
  热路径代码时应移入本清单（上游维护约定，见 W6 残项审计报告）。

豁免机制（行级，须写明理由）：
  行尾注释 `// audit-allow: <tag> <理由>`，tag 建议取 cold-path / ctor-once /
  amortized。裸 `audit-allow`（无冒号或无理由）视为违规——没有理由的豁免
  等于放宽白名单。豁免数按文件与 tag 统计并随审计输出，供验收留痕。

匹配在剥离注释后的代码文本上进行（避免注释语句误伤）；字符串字面量不剥离
（本区代码字符串极少，误伤时用行级豁免并写明）。
"""
import pathlib
import re
import sys

# 目录递归覆盖（§18 字面审计区）
HOT_DIRS = [
    "src/api",
    "src/core/signal",
]

# 存量单文件覆盖（热路径 inline 头，保留防倒退）
HOT_FILES = [
    "include/imobs/event.h",
    "include/imobs/event_ring.h",
]

FORBIDDEN = [
    (r"std::mutex", "锁"),
    (r"\block_guard\b", "锁"),
    (r"\bunique_lock\b", "锁"),
    (r"\bshared_mutex\b", "锁"),
    (r"\bmalloc\s*\(", "堆分配"),
    (r"\bcalloc\s*\(", "堆分配"),
    (r"\brealloc\s*\(", "堆分配"),
    (r"\bnew\b", "堆分配"),
    (r"\bmake_unique\b", "堆分配"),  # new 的现代拼写：漏掉即审计穿孔
    (r"\bmake_shared\b", "堆分配"),
    (r"std::string\b", "堆类型"),
    (r"std::vector\b", "堆类型"),
    (r"std::map\b", "堆类型"),
    (r"unordered_map", "堆类型"),
    (r"\bsleep_for\b", "阻塞"),
    (r"\bprintf\b", "系统调用"),
    (r"<iostream>", "系统调用"),
    (r"\bmsync\b|\bwrite\s*\(|\bread\s*\(", "系统调用"),
]

# 豁免标注：audit-allow: <tag> <理由>（理由至少一个非空 token）
ALLOW_RE = re.compile(r"audit-allow:\s*(\S+)\s+(\S.*)")
ALLOW_BARE_RE = re.compile(r"audit-allow")


def strip_comments(lines):
    """剥离 // 与 /* */ 注释，返回等长的代码文本列表（行号对齐）。"""
    out = []
    in_block = False
    for line in lines:
        code = []
        i = 0
        n = len(line)
        while i < n:
            if in_block:
                j = line.find("*/", i)
                if j < 0:
                    i = n
                else:
                    in_block = False
                    i = j + 2
                continue
            if line.startswith("//", i):
                break
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            code.append(line[i])
            i += 1
        out.append("".join(code))
    return out


def collect_files(root: pathlib.Path):
    files = []
    for d in HOT_DIRS:
        base = root / d
        if not base.is_dir():
            files.append((d, None))  # 目录缺失也要红：审计清单与代码同步维护
            continue
        for p in sorted(base.rglob("*")):
            if p.suffix in (".h", ".cc", ".cpp", ".hpp"):
                files.append((str(p.relative_to(root)), p))
    for rel in HOT_FILES:
        p = root / rel
        files.append((rel, p if p.exists() else None))
    return files


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    hits = []
    exemptions = []  # (rel, ln, tag, reason, patterns)
    preventive = 0   # 标注了 audit-allow 但当前行未命中任何模式（预防性标注）
    per_file = {}
    scanned = 0

    for rel, path in collect_files(root):
        if path is None:
            hits.append(f"{rel}: 热路径文件/目录缺失（审计清单需要与代码同步维护）")
            continue
        scanned += 1
        raw_lines = path.read_text(encoding="utf-8").splitlines()
        code_lines = strip_comments(raw_lines)
        for ln, (raw, code) in enumerate(zip(raw_lines, code_lines), 1):
            matched = [why for pat, why in FORBIDDEN if re.search(pat, code)]
            allow = ALLOW_RE.search(raw)
            if matched:
                if allow:
                    tag, reason = allow.group(1), allow.group(2).strip()
                    exemptions.append((rel, ln, tag, reason, matched))
                    per_file[rel] = per_file.get(rel, 0) + 1
                elif ALLOW_BARE_RE.search(raw):
                    hits.append(f"{rel}:{ln}: [豁免缺理由] 裸 audit-allow 不豁免，"
                                f"须写 `audit-allow: <tag> <理由>`：{raw.strip()}")
                else:
                    hits.append(f"{rel}:{ln}: [{'/'.join(sorted(set(matched)))}] {raw.strip()}")
            elif allow or ALLOW_BARE_RE.search(raw):
                preventive += 1  # 不红：语义性豁免（如 reserve 类分配不在正则清单内）

    if hits:
        print("热路径审计失败：")
        for h in hits:
            print("  " + h)
        return 1

    by_tag = {}
    for _, _, tag, _, _ in exemptions:
        by_tag[tag] = by_tag.get(tag, 0) + 1
    tag_stat = " / ".join(f"{t} {c}" for t, c in sorted(by_tag.items())) or "无"
    print(f"热路径审计通过：{scanned} 个文件，0 违规；"
          f"豁免 {len(exemptions)} 行（{tag_stat}），预防性标注 {preventive} 行")
    for rel in sorted(per_file):
        print(f"  豁免 {per_file[rel]:2d} 行  {rel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
