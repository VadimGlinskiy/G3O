import pefile

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Game.dll')
rva = 0xCA810
for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if exp.address == rva:
        print(f"Game.dll 0x{rva:X}: {exp.name.decode('utf-8')}")
        break
