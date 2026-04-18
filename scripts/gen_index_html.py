#!/usr/bin/env python3
"""Generate src/index_html.c from src/index.html.

PlatformIO extra_script: regenerates only if the HTML is newer.
"""
import os
from pathlib import Path

ROOT = Path(os.environ.get("PROJECT_DIR", Path.cwd())).resolve()
HTML = ROOT / "src" / "index.html"
OUT = ROOT / "src" / "index_html.c"


def main() -> int:
    if not HTML.exists():
        return 0
    if OUT.exists() and OUT.stat().st_mtime >= HTML.stat().st_mtime:
        return 0
    data = HTML.read_bytes()
    bytes_per_line = 16
    lines = []
    for i in range(0, len(data), bytes_per_line):
        chunk = data[i : i + bytes_per_line]
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    body = "\n".join(lines)
    out = (
        '// Auto-generated from index.html by scripts/gen_index_html.py. Do not edit.\n'
        '#include <stddef.h>\n\n'
        'const char index_html_data[] = {\n'
        f'{body}\n'
        '  0x00\n'
        '};\n'
        f'const size_t index_html_len = {len(data)};\n'
    )
    OUT.write_text(out)
    print(f"gen_index_html: wrote {OUT.name} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    # Allow standalone invocation.
    raise SystemExit(main())


# PlatformIO hook.
try:
    Import("env")  # type: ignore  # noqa: F821
    project_dir = env["PROJECT_DIR"]  # type: ignore  # noqa: F821
    ROOT = Path(project_dir).resolve()
    HTML = ROOT / "src" / "index.html"
    OUT = ROOT / "src" / "index_html.c"
    main()
except NameError:
    pass
