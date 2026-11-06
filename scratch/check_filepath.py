import pefile

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')
rvadict = {}
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if exp.name and 'GetFilePath' in exp.name.decode('utf-8', errors='ignore'):
            print(hex(exp.address), exp.name.decode('utf-8', errors='ignore'))
