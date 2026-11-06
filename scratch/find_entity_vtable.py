import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
import struct

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')

# Search for ??_7eCEntity@@6B@ in exports
for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if exp.name and b'??_7eCEntity@@6B@' in exp.name:
        vtable_rva = exp.address
        print(f"eCEntity vtable at RVA 0x{vtable_rva:X}")
        # read offset 0xb0
        offset_in_pe = pe.get_offset_from_rva(vtable_rva + 0xB0)
        data = pe.get_data(vtable_rva + 0xB0, 4)
        target_va = struct.unpack('<I', data)[0]
        target_rva = target_va - pe.OPTIONAL_HEADER.ImageBase
        print(f"vtable[0xB0] = VA 0x{target_va:X} (RVA 0x{target_rva:X})")
        break
