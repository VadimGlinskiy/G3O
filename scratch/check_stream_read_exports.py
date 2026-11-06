import pefile

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\SharedBase.dll')

for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if exp.name and b'?Read@bCIStream' in exp.name:
        print(hex(exp.address), exp.name.decode('utf-8', errors='ignore'))
