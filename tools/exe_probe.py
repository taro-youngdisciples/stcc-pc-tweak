"""STCC.EXE の機械的な下調べ。

  .venv\\Scripts\\python tools\\exe_probe.py <STCC.EXE> [<STCC.EXE> ...]

出力:
  - PE 基本情報（リンク日時、ImageBase、.reloc の有無＝アドレス固定かどうか）
  - 埋め込まれた DirectX 系 GUID（どのインターフェース世代を QueryInterface しているか）
  - DirectInputCreateA に渡している DINPUT_VERSION
  - アスペクト比・解像度まわりの候補定数（float / double / 16.16 固定小数点）の出現位置
"""

from __future__ import annotations

import struct
import sys
import uuid
from pathlib import Path

import capstone
import pefile

GUIDS = {
    # DirectDraw
    "IID_IDirectDraw":          "6C14DB80-A733-11CE-A521-0020AF0BE560",
    "IID_IDirectDraw2":         "B3A6F3E0-2B43-11CF-A2DE-00AA00B93356",
    "IID_IDirectDraw4":         "9C59509A-39BD-11D1-8C4A-00C04FD930C5",
    "IID_IDirectDrawSurface2":  "57805885-6EEC-11CF-9441-A82303C10E27",
    "IID_IDirectDrawSurface3":  "DA044E00-69B2-11D0-A1D5-00AA00B8DFBB",
    # Direct3D Immediate Mode
    "IID_IDirect3D":            "3BBA0080-2421-11CF-A31A-00AA00B93356",
    "IID_IDirect3D2":           "6AAE1EC1-662A-11D0-889D-00AA00BBB76A",
    "IID_IDirect3D3":           "BB223240-E72B-11D0-A9B4-00AA00C0993E",
    "IID_IDirect3DDevice2":     "93281501-8CF8-11D0-89AB-00A0C9054129",
    "IID_IDirect3DHALDevice":   "84E63DE0-46AA-11CF-816F-0000C020156E",
    "IID_IDirect3DRGBDevice":   "A4665C60-2673-11CF-A31A-00AA00B93356",
    "IID_IDirect3DRampDevice":  "F2086B20-259F-11CF-A31A-00AA00B93356",
    "IID_IDirect3DMMXDevice":   "881949A1-D6F3-11D0-89AB-00A0C9054129",
    "IID_IDirect3DTexture2":    "93281502-8CF8-11D0-89AB-00A0C9054129",
    # DirectInput
    "IID_IDirectInput2A":       "5944E662-AA8A-11CF-BFC7-444553540000",
    "IID_IDirectInputDeviceA":  "5944E680-C92E-11CF-BFC7-444553540000",
    "IID_IDirectInputDevice2A": "5944E682-C92E-11CF-BFC7-444553540000",
    "GUID_SysKeyboard":         "6F1D2B61-D5A0-11CF-BFC7-444553540000",
    "GUID_SysMouse":            "6F1D2B60-D5A0-11CF-BFC7-444553540000",
    "GUID_Joystick":            "6F1D2B70-D5A0-11CF-BFC7-444553540000",
    "GUID_ConstantForce":       "13541C20-8E33-11D0-9AD0-00A0C9A06E35",
    "GUID_Spring":              "13541C27-8E33-11D0-9AD0-00A0C9A06E35",
    "GUID_Damper":              "13541C28-8E33-11D0-9AD0-00A0C9A06E35",
    # DirectSound
    "IID_IDirectSound3DBuffer": "279AFA86-4981-11CE-A521-0020AF0BE560",
    "IID_IDirectSound3DListener": "279AFA84-4981-11CE-A521-0020AF0BE560",
}

CONSTANTS = {
    "float 4/3":          struct.pack("<f", 4 / 3),
    "float 3/4":          struct.pack("<f", 0.75),
    "double 4/3":         struct.pack("<d", 4 / 3),
    "double 3/4":         struct.pack("<d", 0.75),
    "fixed16.16 4/3":     struct.pack("<i", 0x15555),
    "fixed16.16 3/4":     struct.pack("<i", 0xC000),
    "float 640":          struct.pack("<f", 640.0),
    "float 480":          struct.pack("<f", 480.0),
    "float 320":          struct.pack("<f", 320.0),
    "float 240":          struct.pack("<f", 240.0),
    "double 640":         struct.pack("<d", 640.0),
    "double 480":         struct.pack("<d", 480.0),
}


def section_of(pe: pefile.PE, rva: int) -> str:
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.Name.rstrip(b"\0").decode()
    return "?"


def probe(path: Path) -> None:
    pe = pefile.PE(str(path))
    data = path.read_bytes()
    base = pe.OPTIONAL_HEADER.ImageBase
    print(f"==== {path}")
    print(f"  size={len(data)}  ImageBase=0x{base:08X}  "
          f"linked={pe.FILE_HEADER.TimeDateStamp:#x}  "
          f".reloc={'yes' if any(s.Name.startswith(b'.reloc') for s in pe.sections) else 'NO (アドレス固定)'}  "
          f"Characteristics=0x{pe.FILE_HEADER.Characteristics:04X}")

    print("  -- DirectX GUIDs --")
    for name, g in GUIDS.items():
        needle = uuid.UUID(g).bytes_le
        hits = []
        start = 0
        while (i := data.find(needle, start)) != -1:
            rva = pe.get_rva_from_offset(i)
            hits.append(f"0x{base + rva:08X}({section_of(pe, rva)})" if rva is not None else f"off:0x{i:X}")
            start = i + 1
        if hits:
            print(f"    {name:28s} {', '.join(hits)}")

    print("  -- DirectInputCreateA の呼び出しと直前の push --")
    iat = {imp.address: imp.name.decode() for entry in pe.DIRECTORY_ENTRY_IMPORT
           for imp in entry.imports if imp.name}
    dic = [a for a, n in iat.items() if n == "DirectInputCreateA"]
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    code = text.get_data()
    text_va = base + text.VirtualAddress
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False
    insns = list(md.disasm(code, text_va))
    for idx, ins in enumerate(insns):
        if ins.mnemonic == "call" and any(f"0x{a:x}" in ins.op_str for a in dic):
            ctx = insns[max(0, idx - 6):idx + 1]
            print("    " + " | ".join(f"{i.address:08X}: {i.mnemonic} {i.op_str}" for i in ctx))

    print("  -- 候補定数 --")
    for name, needle in CONSTANTS.items():
        hits = []
        start = 0
        while (i := data.find(needle, start)) != -1:
            rva = pe.get_rva_from_offset(i)
            if rva is not None:
                sec = section_of(pe, rva)
                if sec != ".text" or len(needle) >= 8:   # 4byte 値の .text 内ヒットは命令の一部であることが多いので件数だけ
                    hits.append(f"0x{base + rva:08X}({sec})")
                else:
                    hits.append("text")
            start = i + 1
        text_hits = hits.count("text")
        others = [h for h in hits if h != "text"]
        summary = ", ".join(others[:12]) + (f" ...(+{len(others) - 12})" if len(others) > 12 else "")
        print(f"    {name:18s} data={len(others):3d} text={text_hits:3d}  {summary}")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        probe(Path(p))
