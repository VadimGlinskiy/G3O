import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')
md = Cs(CS_ARCH_X86, CS_MODE_32)

target_va = 0x10602a88
text_sec = None
for s in pe.sections:
    if s.Name.startswith(b'.text'):
        text_sec = s
        break

code = text_sec.get_data()
base = pe.OPTIONAL_HEADER.ImageBase + text_sec.VirtualAddress

refs = []
for ins in md.disasm(code, base):
    if "0x10602a88" in ins.op_str:
        refs.append((ins.address, ins.mnemonic, ins.op_str))

print(f"Found {len(refs)} references to 0x10602a88:")
for addr, mnem, op in refs[:10]:
    print(f"  0x{addr:X}: {mnem} {op}")
