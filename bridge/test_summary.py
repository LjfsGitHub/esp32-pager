"""Unit tests for device-screen tool/approval summaries.

The 1.83" screen (240x284) only gets one-line human labels: no raw pipelines,
no full paths, no command bodies.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from pager_bridge.bridge import approval_summary, tool_summary  # noqa: E402

CASES = [
    ("terminal: taskkill /FO CSV /NH 2>/dev/null | awk -F, '{print $1}'", "execute"),
    ("terminal: echo === CPU/MEM === + 3 commands", "execute"),
    ("execute_code: print(\"HELLO_G0\")", "execute"),
    ("write_file: pager_g0_round2.txt", "edit"),
    ("search_files: *", "search"),
    ("read_file: C:/Users/Public/Documents/demo/very/long/path/notes.txt", "read"),
]

failures = 0
for title, kind in CASES:
    out = tool_summary(title, kind)
    ok = len(out) <= 40 and "/FO" not in out and "==" not in out and out and "·" in out
    if not ok:
        failures += 1
    print(f"{'PASS' if ok else 'FAIL'}  {ascii(out)}")

# Approval cards are even tighter: basename for files, first token for commands.
APPROVAL_CASES = [
    (
        "Approve edit: C:\\Users\\Public\\AppData\\Local\\hermes\\cache\\scratch\\top3.ps1",
        "edit",
        "",
        ["C:\\Users\\Public\\AppData\\Local\\hermes\\cache\\scratch\\top3.ps1"],
        "修改文件 · top3.ps1",
    ),
    (
        "Approve command: rm -rf pager_workspace/trash_demo",
        "execute",
        "rm -rf pager_workspace/trash_demo",
        [],
        "运行命令 · rm",
    ),
    (
        "Approve edit: pager_g0_round2.txt",
        "edit",
        "",
        [],
        "改文件 · pager_g0_round2.txt",
    ),
]

for title, kind, command, paths, expected in APPROVAL_CASES:
    out = approval_summary(title, kind, command, paths)
    ok = out == expected and len(out) <= 32
    if not ok:
        failures += 1
    print(f"{'PASS' if ok else 'FAIL'}  {ascii(out)}  expected={ascii(expected)}")

total = len(CASES) + len(APPROVAL_CASES)
print(f"\n{total - failures}/{total} passed")
sys.exit(1 if failures else 0)
