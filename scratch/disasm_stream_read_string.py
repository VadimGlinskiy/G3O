import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\SharedBase.dll')
md = Cs(CS_ARCH_X86, CS_MODE_32)

target_name = b'?Read@bCIStream@@UAE_NAAVbCString@@@Z'
for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if exp.name == target_name:
        rva = exp.address
        print(f"Read(bCString&) at RVA 0x{rva:X}")
        code = pe.get_data(rva, 64)
        for ins in md.disasm(code, 0x10000000 + rva):
            print(f"  0x{ins.address:X}:  {ins.mnemonic} {ins.op_str}")
            if ins.mnemonic == 'ret':
                break
        break
