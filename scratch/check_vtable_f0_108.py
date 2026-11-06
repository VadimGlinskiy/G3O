import pefile
import struct

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')
vtable_rva = 0x4756A4

for offset in [0xF0, 0x104, 0x108]:
    data = pe.get_data(vtable_rva + offset, 4)
    target_va = struct.unpack('<I', data)[0]
    target_rva = target_va - pe.OPTIONAL_HEADER.ImageBase
    print(f"eCEntity vtable[0x{offset:X}] = VA 0x{target_va:X} (RVA 0x{target_rva:X})")
