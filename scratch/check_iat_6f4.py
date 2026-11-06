import pefile

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')

iat_rva = 0x3966F4
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    for imp in entry.imports:
        if imp.address == (pe.OPTIONAL_HEADER.ImageBase + iat_rva):
            name = imp.name.decode('utf-8', errors='ignore') if imp.name else str(imp.ordinal)
            print(f"Import 6F4: {entry.dll.decode('utf-8')} -> {name}")
