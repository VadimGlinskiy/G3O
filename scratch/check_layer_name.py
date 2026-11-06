import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Game.dll')
md = Cs(CS_ARCH_X86, CS_MODE_32)

rvadict = {}
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if exp.name and 'GetName' in exp.name.decode('utf-8', errors='ignore') and 'layer' in exp.name.decode('utf-8', errors='ignore'):
            print(hex(exp.address), exp.name.decode('utf-8', errors='ignore'))
