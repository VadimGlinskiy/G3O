import pefile
import struct

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')

iat_rva = 0x3966FC
data = pe.get_data(iat_rva, 4)
val = struct.unpack('<I', data)[0]
print(f"IAT [0x{iat_rva:X}] = 0x{val:X}")

# Let's check which import this is
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    dll_name = entry.dll.decode('utf-8', errors='ignore')
    for imp in entry.imports:
        if imp.address == (pe.OPTIONAL_HEADER.ImageBase + iat_rva):
            name = imp.name.decode('utf-8', errors='ignore') if imp.name else str(imp.ordinal)
            print(f"Import: {dll_name} -> {name}")
