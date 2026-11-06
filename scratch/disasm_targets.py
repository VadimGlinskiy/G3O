import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')
md = Cs(CS_ARCH_X86, CS_MODE_32)

targets = [0x21e120, 0x2295c0, 0x2257f0, 0x224350, 0x21a150, 0x224970]

for rva in targets:
    offset = pe.get_offset_from_rva(rva)
    code = pe.get_data(rva, 32)
    print(f"=== RVA 0x{rva:X} (file offset 0x{offset:X}) ===")
    for ins in md.disasm(code, 0x10000000 + rva):
        print(f"0x{ins.address:X}:  {ins.mnemonic} {ins.op_str}")
        if ins.mnemonic in ['ret', 'retf', 'jmp']:
            break
