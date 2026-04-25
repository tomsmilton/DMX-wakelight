#!/usr/bin/env python3
"""Embed HTML files into C arrays for the ESP firmware.

Files are gzipped before embedding; the HTTP handler serves them with
Content-Encoding: gzip so the browser decompresses transparently. Regenerates
src/<symbol>.c only if the source .html is newer.
"""
import gzip
import os
from pathlib import Path

PAGES = [
    ("index.html",  "index_html"),
    ("live.html",   "live_html"),
    ("picker.html", "picker_html"),
]


def gen(root: Path, html_name: str, symbol: str) -> None:
    html = root / "src" / html_name
    out = root / "src" / f"{symbol}.c"
    if not html.exists():
        return
    if out.exists() and out.stat().st_mtime >= html.stat().st_mtime:
        return
    raw = html.read_bytes()
    # mtime=0 keeps gzip deterministic across builds (no rebuild churn).
    data = gzip.compress(raw, compresslevel=9, mtime=0)
    bytes_per_line = 16
    lines = []
    for i in range(0, len(data), bytes_per_line):
        chunk = data[i : i + bytes_per_line]
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    body = "\n".join(lines)
    text = (
        f'// Auto-generated from {html_name} by scripts/gen_index_html.py. Do not edit.\n'
        '// Contents are gzip-compressed; HTTP handler sets Content-Encoding: gzip.\n'
        '#include <stddef.h>\n\n'
        f'const unsigned char {symbol}_data[] = {{\n'
        f'{body}\n'
        '};\n'
        f'const size_t {symbol}_len = {len(data)};\n'
    )
    out.write_text(text)
    print(f"gen_index_html: wrote {out.name} ({len(data)} bytes gz, from {len(raw)} raw)")


def main(root: Path) -> int:
    for html_name, symbol in PAGES:
        gen(root, html_name, symbol)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(Path(os.environ.get("PROJECT_DIR", Path.cwd())).resolve()))


# PlatformIO hook.
try:
    Import("env")  # type: ignore  # noqa: F821
    project_dir = env["PROJECT_DIR"]  # type: ignore  # noqa: F821
    main(Path(project_dir).resolve())
except NameError:
    pass
