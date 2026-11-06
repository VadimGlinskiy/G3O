import pefile

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')

rva = 0x602A88
print(f"Engine.dll RVA 0x{rva:X} in data/bss section")
for s in pe.sections:
    start = s.VirtualAddress
    end = start + s.Misc_VirtualSize
    if start <= rva < end:
        print(f"Section {s.Name.decode('utf-8').strip()}: 0x{start:X} - 0x{end:X}")
