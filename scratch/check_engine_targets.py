import pefile

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')
rvadict = {}
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if exp.name:
            rvadict[exp.address] = exp.name.decode('utf-8', errors='ignore')

targets = [0x21e120, 0x2295c0, 0x2257f0, 0x224350, 0x21a150, 0x224970]
for t in targets:
    print(f"RVA 0x{t:x}: {rvadict.get(t, 'NON-EXPORTED (Internal method)')}")
