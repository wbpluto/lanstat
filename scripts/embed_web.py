#!/usr/bin/env python3
"""Generate C header containing plain and gzip-compressed web dashboard assets."""

import gzip
import json
import pathlib
import sys


def format_c_array(name: str, data: bytes) -> str:
    lines = [f"static const unsigned char {name}[] = {{"]
    for offset in range(0, len(data), 16):
        chunk = ", ".join(f"0x{byte:02x}" for byte in data[offset : offset + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append(f"static const size_t {name}_len = {len(data)}u;")
    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write(f"usage: {sys.argv[0]} <input.html> <output.h>\n")
        return 1

    src_path = pathlib.Path(sys.argv[1])
    out_path = pathlib.Path(sys.argv[2])
    html_text = src_path.read_text(encoding="utf-8")
    oui_path = src_path.parent / "oui_packed.txt"
    if oui_path.exists() and '/*__OUI_DATA__*/""' in html_text:
        oui_text = oui_path.read_text(encoding="utf-8").strip()
        html_text = html_text.replace('/*__OUI_DATA__*/""', json.dumps(oui_text), 1)
    raw = html_text.encode("utf-8")
    gz = gzip.compress(raw, compresslevel=9, mtime=0)

    header = "\n".join(
        [
            "#ifndef LANSTAT_WEB_ASSETS_H",
            "#define LANSTAT_WEB_ASSETS_H",
            "",
            "#include <stddef.h>",
            "",
            format_c_array("web_index_html", raw),
            "",
            format_c_array("web_index_html_gz", gz),
            "",
            "#endif",
            "",
        ]
    )
    out_path.write_text(header, encoding="utf-8")
    print(f"Embedded {src_path.name}: raw={len(raw)} bytes, gzip={len(gz)} bytes -> {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
