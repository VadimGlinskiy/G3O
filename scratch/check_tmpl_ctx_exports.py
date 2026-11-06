import pefile

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')

for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if exp.name and b'eCEntityTemplateContext' in exp.name:
        print(hex(exp.address), exp.name.decode('utf-8', errors='ignore'))
