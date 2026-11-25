#define CROME_WALK_BRIDGE_BUILD
#include "CromeWalkBridge.hpp"

#include <cstdint>
#include <limits>
#include <mutex>

namespace {
std::mutex bridgeMutex;
void* bridgeContext = nullptr;
CromeReadPosition readPosition = nullptr;
CromeRequestWalk requestWalk = nullptr;

bool sumFits(std::int32_t value, int delta) {
    const std::int64_t candidate = static_cast<std::int64_t>(value) + delta;
    return candidate >= std::numeric_limits<std::int32_t>::min() &&
           candidate <= std::numeric_limits<std::int32_t>::max();
}
}

extern "C" int WINAPI CromeWalkBridge_SetCallbacks(
    std::uint32_t protocol, void* context,
    CromeReadPosition read, CromeRequestWalk walk) {
    if (protocol != 1 || !read || !walk)
        return CromeWalkInvalidArgument;
    std::lock_guard<std::mutex> lock(bridgeMutex);
    bridgeContext = context;
    readPosition = read;
    requestWalk = walk;
    return CromeWalkOk;
}

extern "C" int WINAPI CromeWalkBridge_Reset() {
    std::lock_guard<std::mutex> lock(bridgeMutex);
    readPosition = nullptr;
    requestWalk = nullptr;
    bridgeContext = nullptr;
    return CromeWalkOk;
}

extern "C" int WINAPI CromeWalkBridge_WalkRelative(int dx, int dy, int dz) {
    // Teste deliberadamente restrito a um único comando de 1 a 2 SQMs
    // no mesmo andar. Não há fallback para teclado.
    if (dz != 0 || dx < -2 || dx > 2 || dy < -2 || dy > 2 ||
        (dx != 0 && dy != 0) || (dx == 0 && dy == 0))
        return CromeWalkInvalidArgument;

    void* context = nullptr;
    CromeReadPosition read = nullptr;
    CromeRequestWalk walk = nullptr;
    {
        std::lock_guard<std::mutex> lock(bridgeMutex);
        context = bridgeContext;
        read = readPosition;
        walk = requestWalk;
    }
    if (!read || !walk) return CromeWalkNotInitialized;

    CromeWalkPosition from{};
    if (!read(context, &from)) return CromeWalkReadFailed;
    if (!sumFits(from.x, dx) || !sumFits(from.y, dy))
        return CromeWalkInvalidArgument;

    const CromeWalkPosition to{from.x + dx, from.y + dy, from.z};
    return walk(context, &to) ? CromeWalkOk : CromeWalkRequestFailed;
}
