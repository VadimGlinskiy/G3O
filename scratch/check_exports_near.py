import pefile

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\SharedBase.dll')

def find_nearest_export(rva):
    best = None
    best_dist = 999999
    for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if exp.address <= rva:
            dist = rva - exp.address
            if dist < best_dist:
                best_dist = dist
                best = exp
    if best and best.name:
        return best.name.decode('utf-8', errors='ignore'), best_dist
    return "unknown", 0

for rva in [0x5BD30, 0x63810, 0x57FB0, 0xA4DE]:
    name, dist = find_nearest_export(rva)
    print(f"RVA 0x{rva:X} -> {name} + 0x{dist:X}")
