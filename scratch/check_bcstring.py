import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\SharedBase.dll')
md = Cs(CS_ARCH_X86, CS_MODE_32)

rvadict = {}
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if exp.name and 'GetText@bCString' in exp.name.decode('utf-8', errors='ignore'):
            print(hex(exp.address), exp.name.decode('utf-8', errors='ignore'))
            code = pe.get_data(exp.address, 16)
            for ins in md.disasm(code, 0x10000000 + exp.address):
                print(f"  0x{ins.address:X}:  {ins.mnemonic} {ins.op_str}")
