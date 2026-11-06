import pefile
import struct

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')

for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if exp.name and b'??_7eCDynamicEntity@@6B@' in exp.name:
        vtable_rva = exp.address
        print(f"eCDynamicEntity vtable at RVA 0x{vtable_rva:X}")
        for offset in [0xF0, 0x104, 0x108]:
            data = pe.get_data(vtable_rva + offset, 4)
            target_va = struct.unpack('<I', data)[0]
            target_rva = target_va - pe.OPTIONAL_HEADER.ImageBase
            print(f"  vtable[0x{offset:X}] = VA 0x{target_va:X} (RVA 0x{target_rva:X})")
        break
