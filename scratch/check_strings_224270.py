import pefile

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')

for va in [0x10475008, 0x10471258, 0x1042c3a0]:
    rva = va - pe.OPTIONAL_HEADER.ImageBase
    data = pe.get_data(rva, 64)
    # read null-terminated string
    s = data.split(b'\0')[0].decode('utf-8', errors='ignore')
    print(f"VA 0x{va:X}: {s}")
