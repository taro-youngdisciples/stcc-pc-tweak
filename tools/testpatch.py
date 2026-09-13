"""調査用の使い捨てパッチを、exe の「別名コピー」に当てる。

原本は変更しない。確定した変更は必ずラッパーDLL側へ移すこと（HANDOFF §3 決定2）。

  .venv\\Scripts\\python tools\\testpatch.py list
  .venv\\Scripts\\python tools\\testpatch.py apply windowed [--src D:\\Games\\STCC\\STCC.EXE] [--out D:\\Games\\STCC\\STCC_test.EXE]
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

import pefile

SUPPORTED_SHA256 = "6B4E92CA0A4C156363435A3425DA5704B2E64E9EF2218CF51449F9AA42C4319D"  # jp-1.02

# name -> list of (VA, 元のバイト列, 新しいバイト列, 説明)
PATCHES: dict[str, list[tuple[int, bytes, bytes, str]]] = {
    "windowed": [
        (
            0x0043964D,
            bytes.fromhex("C7 05 30 89 56 00 01 00 00 00"),
            bytes.fromhex("C7 05 30 89 56 00 00 00 00 00"),
            "InitInstance: mov [g_bFullscreen], 1 -> 0（ウィンドウモードで起動）",
        ),
    ],
}


def apply(name: str, data: bytearray, pe: pefile.PE) -> None:
    base = pe.OPTIONAL_HEADER.ImageBase
    for va, old, new, desc in PATCHES[name]:
        off = pe.get_offset_from_rva(va - base)
        cur = bytes(data[off:off + len(old)])
        if cur != old:
            sys.exit(f"0x{va:08X}: 元のバイト列が一致しません ({cur.hex(' ')})")
        data[off:off + len(new)] = new
        print(f"  0x{va:08X} (file 0x{off:X}): {old.hex(' ')} -> {new.hex(' ')}  {desc}")


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    a = sub.add_parser("apply")
    a.add_argument("patches", nargs="+", choices=sorted(PATCHES))
    a.add_argument("--src", type=Path, default=Path(r"D:\Games\STCC\STCC.EXE"))
    a.add_argument("--out", type=Path, default=Path(r"D:\Games\STCC\STCC_test.EXE"))
    args = ap.parse_args()

    if args.cmd == "list":
        for name, items in PATCHES.items():
            print(name)
            for va, _, _, desc in items:
                print(f"  0x{va:08X}  {desc}")
        return

    if args.out.resolve() == args.src.resolve():
        sys.exit("出力先が原本と同じです。別名にしてください")
    data = bytearray(args.src.read_bytes())
    digest = hashlib.sha256(data).hexdigest().upper()
    if digest != SUPPORTED_SHA256:
        sys.exit(f"未対応の exe です: {digest}")
    pe = pefile.PE(data=bytes(data))
    for name in args.patches:
        print(f"[{name}]")
        apply(name, data, pe)
    args.out.write_bytes(data)
    print(f"作成: {args.out}  sha256={hashlib.sha256(data).hexdigest().upper()}")


if __name__ == "__main__":
    main()
