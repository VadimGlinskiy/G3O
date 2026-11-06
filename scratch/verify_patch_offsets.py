import pefile

pe = pefile.PE(r'E:\SteamLibrary\steamapps\common\Gothic 3\Engine.dll')
base = pe.OPTIONAL_HEADER.ImageBase

def check_addr(rva, name):
    print(f"{name}: RVA 0x{rva:X}, VA 0x{base + rva:X}")

check_addr(0x17B20, "s_getTemplateContext")
check_addr(0x1F0F20, "s_findNode")
check_addr(0x602A88, "s_engineGlobalPtr")
check_addr(0x224350, "eCEntity::PatchWithTemplate")
