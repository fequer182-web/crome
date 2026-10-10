#pragma once
#include <windows.h>
#include <cstdint>
#include <limits>

namespace crome::olders_attack {
// Exact Olders baseline: the natural caller increments Creature+8 once for
// its by-value argument; Game::attack decrements it on every normal exit.
// This is not a generic std::shared_ptr control block or a borrowed argument.
inline bool acquireConsumedReference(volatile LONG* counter) {
    LONG previous=InterlockedCompareExchange(counter,0,0);
    for(;;){
        if(previous<=0||previous==std::numeric_limits<LONG>::max())return false;
        const LONG observed=InterlockedCompareExchange(counter,previous+1,previous);
        if(observed==previous)return true;
        previous=observed;
    }
}
inline constexpr std::uintptr_t counterOffset=8;
}
