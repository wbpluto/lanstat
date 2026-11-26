#!/usr/bin/env python3
import pathlib
import struct
import sys


def sfh_hash(data: bytes) -> int:
    length = len(data)
    if length <= 0:
        return 0
    h = length & 0xFFFFFFFF
    rem = length & 3
    n = length >> 2
    idx = 0
    for _ in range(n):
        h = (h + (data[idx] | (data[idx + 1] << 8))) & 0xFFFFFFFF
        tmp = ((((data[idx + 2] | (data[idx + 3] << 8)) << 11) & 0xFFFFFFFF) ^ h) & 0xFFFFFFFF
        h = (((h << 16) & 0xFFFFFFFF) ^ tmp) & 0xFFFFFFFF
        idx += 4
        h = (h + (h >> 11)) & 0xFFFFFFFF

    if rem == 3:
        h = (h + (data[idx] | (data[idx + 1] << 8))) & 0xFFFFFFFF
        h = (h ^ ((h << 16) & 0xFFFFFFFF)) & 0xFFFFFFFF
        b2 = data[idx + 2]
        if b2 >= 128:
            b2 -= 256
        h = (h ^ ((b2 << 18) & 0xFFFFFFFF)) & 0xFFFFFFFF
        h = (h + (h >> 11)) & 0xFFFFFFFF
    elif rem == 2:
        h = (h + (data[idx] | (data[idx + 1] << 8))) & 0xFFFFFFFF
        h = (h ^ ((h << 11) & 0xFFFFFFFF)) & 0xFFFFFFFF
        h = (h + (h >> 17)) & 0xFFFFFFFF
    elif rem == 1:
        b0 = data[idx]
        if b0 >= 128:
            b0 -= 256
        h = (h + b0) & 0xFFFFFFFF
        h = (h ^ ((h << 10) & 0xFFFFFFFF)) & 0xFFFFFFFF
        h = (h + (h >> 1)) & 0xFFFFFFFF

    h = (h ^ ((h << 3) & 0xFFFFFFFF)) & 0xFFFFFFFF
    h = (h + (h >> 5)) & 0xFFFFFFFF
    h = (h ^ ((h << 4) & 0xFFFFFFFF)) & 0xFFFFFFFF
    h = (h + (h >> 17)) & 0xFFFFFFFF
    h = (h ^ ((h << 25) & 0xFFFFFFFF)) & 0xFFFFFFFF
    h = (h + (h >> 6)) & 0xFFFFFFFF
    return h


def build_lmo(entries: dict[str, str]) -> bytes:
    payload = bytearray()
    index = []
    for key, val in entries.items():
        kbytes = key.encode("utf-8")
        vbytes = val.encode("utf-8")
        offset = len(payload)
        payload.extend(vbytes)
        pad = (4 - (len(payload) % 4)) % 4
        if pad:
            payload.extend(b"\x00" * pad)
        index.append((sfh_hash(kbytes), sfh_hash(vbytes), offset, len(vbytes)))

    index.sort(key=lambda item: item[0])
    table_offset = len(payload)
    out = bytearray(payload)
    for key_id, val_id, offset, length in index:
        out.extend(struct.pack(">IIII", key_id, val_id, offset, length))
    out.extend(struct.pack(">I", table_offset))
    return bytes(out)


CATALOGS = {
    "zh-cn": {
        "lanstat": "实时带宽监控",
        "Lightweight real-time LAN bandwidth monitor.": "轻量级原生局域网实时带宽监控仪表盘。",
        "Open lanstat Dashboard": "打开实时带宽监控页面",
        "Open in New Tab": "新窗口打开",
        "HTTPS LuCI cannot embed the HTTP dashboard directly. Open it in a new tab instead.": "当前通过 HTTPS 访问 LuCI，浏览器限制无法直接内嵌 HTTP 面板，请在新标签页打开。",
    },
    "zh-tw": {
        "lanstat": "即時頻寬監控",
        "Lightweight real-time LAN bandwidth monitor.": "輕量級原生區域網路即時頻寬監控儀表板。",
        "Open lanstat Dashboard": "開啟即時頻寬監控頁面",
        "Open in New Tab": "在新視窗開啟",
        "HTTPS LuCI cannot embed the HTTP dashboard directly. Open it in a new tab instead.": "目前透過 HTTPS 存取 LuCI，瀏覽器限制無法直接內嵌 HTTP 面板，請在新分頁開啟。",
    },
}


def main() -> int:
    out_dir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path("openwrt/usr/lib/lua/luci/i18n")
    out_dir.mkdir(parents=True, exist_ok=True)
    for lang, entries in CATALOGS.items():
        data = build_lmo(entries)
        dest = out_dir / f"lanstat.{lang}.lmo"
        dest.write_bytes(data)
        print(f"Generated {dest} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
