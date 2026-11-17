#!/usr/bin/env python3
# HTML 是唯一维护来源；--check 用于阻止固件携带过期的内嵌网页。
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / "tools/web_auth_companion.html"
    target = root / "main/auth_web_page.h"
    text = source.read_text(encoding="utf-8")
    content = "/* 自动生成：请修改 tools/web_auth_companion.html 后运行生成器。 */\n#pragma once\n\nstatic const char s_web_page_html[] =\n"
    content += "\n".join("    " + json.dumps(line, ensure_ascii=True) for line in text.splitlines(keepends=True))
    content += ";\n#define s_web_page_html_len (sizeof(s_web_page_html) - 1)\n"
    if args.check:
        if target.read_text(encoding="utf-8") != content:
            parser.exit(1, "Embedded page is stale; run python3 tools/embed_web_page.py\n")
        print("Embedded web page: PASS")
    else:
        target.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
