#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CromeWalkBridge.hpp"
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <iostream>

struct TestWorld {
    CromeWalkPosition player{32963, 32109, 6};
    int requests = 0;
};
bool WINAPI readMock(void* ptr, CromeWalkPosition* out) {
    if (!ptr || !out) return false;
    *out = static_cast<TestWorld*>(ptr)->player;
    return true;
}
bool WINAPI walkMock(void* ptr, const CromeWalkPosition* destination) {
    if (!ptr || !destination) return false;
    auto* world = static_cast<TestWorld*>(ptr);
    world->requests++;
    std::printf("[MOCK] Pedido de destino: %d,%d,%d\n",
                destination->x, destination->y, destination->z);
    world->player = *destination;
    return true;
}

int main() {
    // DLL carregada em NOSSO processo de teste. Nao usa OpenProcess,
    // WriteProcessMemory, CreateRemoteThread ou cliente Antica.
    HMODULE dll = LoadLibraryW(L"CromeWalkBridge_x64.dll");
    if (!dll) {
        std::cerr << "[FAIL] LoadLibrary falhou: " << GetLastError() << '\n';
        return 1;
    }
    using SetFn = int(WINAPI*)(std::uint32_t, void*, CromeReadPosition, CromeRequestWalk);
    using WalkFn = int(WINAPI*)(int, int, int);
    using ResetFn = int(WINAPI*)();

    auto set = reinterpret_cast<SetFn>(GetProcAddress(dll,"CromeWalkBridge_SetCallbacks"));
    auto walk = reinterpret_cast<WalkFn>(GetProcAddress(dll,"CromeWalkBridge_WalkRelative"));
    auto reset = reinterpret_cast<ResetFn>(GetProcAddress(dll,"CromeWalkBridge_Reset"));
    if (!set || !walk || !reset) {
        std::cerr << "[FAIL] Exports ausentes\n";
        FreeLibrary(dll);
        return 2;
    }
    TestWorld world;
    const auto before = world.player;
    std::printf("[MOCK] ANTES  : %d,%d,%d\n", before.x, before.y, before.z);

    if (walk(2,0,0) != CromeWalkNotInitialized) {
        std::cerr << "[FAIL] Bridge aceitou comando sem callbacks\n";
        FreeLibrary(dll);
        return 3;
    }
    if (set(1, &world, &readMock, &walkMock) != CromeWalkOk) {
        std::cerr << "[FAIL] Falha ao registrar callbacks\n";
        FreeLibrary(dll);
        return 4;
    }
    const int status = walk(2,0,0);
    std::printf("[MOCK] DEPOIS : %d,%d,%d\n",
                world.player.x, world.player.y, world.player.z);
    const bool ok = status == CromeWalkOk &&
                    world.player.x == before.x + 2 &&
                    world.player.y == before.y &&
                    world.player.z == before.z &&
                    world.requests == 1 &&
                    walk(0, 0, 1) == CromeWalkInvalidArgument;
    reset();
    FreeLibrary(dll);
    std::cout << (ok ? "[OK] DLL x64, API e WALK_REL(2,0,0) validados no HOST DE TESTE.\n"
                     : "[FAIL] Resposta inesperada da bridge.\n");
    std::cout << "[INFO] Nenhum personagem real foi movido.\n";
    return ok ? 0 : 5;
}
