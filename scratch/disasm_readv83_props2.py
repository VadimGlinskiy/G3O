import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')
md = Cs(CS_ARCH_X86, CS_MODE_32)

rva = 0x2261EC
code = pe.get_data(rva, 400)
print(f"=== Engine.dll RVA 0x{rva:X} ===")
for ins in md.disasm(code, 0x10000000 + rva):
    print(f"0x{ins.address:X}:  {ins.mnemonic} {ins.op_str}")
    if ins.mnemonic == 'ret':
        print(f"0x{ins.address:X}:  {ins.mnemonic} {ins.op_str}")
        break
