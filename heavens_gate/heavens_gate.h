#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

namespace HeavensGate {

    // Проверка, запущен ли процесс под WOW64 (в 64-битной Windows)
    bool IsSupported();

    // Инициализация модуля: поиск 64-битной ntdll и подготовка шлюза
    bool Initialize();

    // Универсальный вызов 64-битной функции из 32-битного кода
    // Поддерживает соглашение о вызовах Microsoft x64 (RCX, RDX, R8, R9, стек)
    uint64_t Call64(uint64_t funcAddr, size_t numArgs, ...);

    // Получение адреса экспортированной функции из 64-битной ntdll.dll
    uint64_t GetNtdll64ProcAddress(const char* procName);

    // Выделение виртуальной памяти выше границы 4 ГБ (> 0xFFFFFFFF)
    // Возвращает 64-битный адрес выделенного блока памяти
    uint64_t AllocateHighMemory(size_t sizeInBytes);

    // Освобождение ранее выделенной 64-битной памяти
    bool FreeHighMemory(uint64_t addr64);

    // Копирование данных из 64-битной памяти (>4 ГБ) в 32-битный буфер игры (<4 ГБ)
    bool ReadHighMemory(void* dst32, uint64_t src64, size_t bytes);

    // Копирование данных из 32-битного буфера игры (<4 ГБ) в 64-битную память (>4 ГБ)
    bool WriteHighMemory(uint64_t dst64, const void* src32, size_t bytes);

} // namespace HeavensGate
