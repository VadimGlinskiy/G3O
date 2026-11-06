import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\SharedBase.dll')
md = Cs(CS_ARCH_X86, CS_MODE_32)

# search for call 0x10057fb0 in SharedBase
target_va = 0x10057fb0
text_sec = None
for s in pe.sections:
    if s.Name.startswith(b'.text'):
        text_sec = s
        break

code = text_sec.get_data()
base = pe.OPTIONAL_HEADER.ImageBase + text_sec.VirtualAddress

callers = []
for ins in md.disasm(code, base):
    if ins.mnemonic == 'call':
        try:
            addr = int(ins.op_str, 16)
            if addr == target_va:
                callers.append(ins.address)
        except:
            pass

print(f"Found {len(callers)} callers to 0x10057fb0:")
for c in callers:
    print(f"  Caller at VA 0x{c:X} (RVA 0x{c - pe.OPTIONAL_HEADER.ImageBase:X})")
