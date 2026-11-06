import pefile

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')
data = open(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll', 'rb').read()

pattern = b'\x88\x2a\x60\x10'
idx = 0
found = []
while True:
    pos = data.find(pattern, idx)
    if pos == -1:
        break
    found.append(pos)
    idx = pos + 1

print(f"Found {len(found)} references:")
for pos in found:
    rva = pe.get_rva_from_offset(pos)
    print(f"  Offset 0x{pos:X} (RVA 0x{rva:X})")
