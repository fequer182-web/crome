#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

#ifdef CROME_WALK_BRIDGE_BUILD
#define CROME_WALK_API __declspec(dllexport)
#else
#define CROME_WALK_API
#endif

// Contrato C estável entre um cliente que CARREGA a DLL de forma autorizada
// e a bridge. Não há acesso externo a processos, hooks ou RVAs aqui.
struct CromeWalkPosition {
    std::int32_t x, y, z;
};

using CromeReadPosition = bool (WINAPI*)(void* context, CromeWalkPosition* out);
using CromeRequestWalk = bool (WINAPI*)(void* context, const CromeWalkPosition* destination);

enum CromeWalkResult : int {
    CromeWalkOk = 0,
    CromeWalkInvalidArgument = 1,
    CromeWalkNotInitialized = 2,
    CromeWalkReadFailed = 3,
    CromeWalkRequestFailed = 4
};

extern "C" {
CROME_WALK_API int WINAPI CromeWalkBridge_SetCallbacks(
    std::uint32_t protocol, void* context,
    CromeReadPosition readPosition, CromeRequestWalk requestWalk);
CROME_WALK_API int WINAPI CromeWalkBridge_WalkRelative(int dx, int dy, int dz);
CROME_WALK_API int WINAPI CromeWalkBridge_Reset();
}
