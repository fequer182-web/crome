#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MiniJson.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

static_assert(sizeof(void*) == 4, "CromeRamoniaBridge must be built as x86");

HMODULE g_module{};
volatile LONG g_running = 1;

std::string g_status = "starting";
std::string g_profileError;
std::uintptr_t g_base{};
std::uint32_t g_imageTimestamp{};
std::uint32_t g_imageSize{};
bool g_buildMatched{false};

HWND g_gameWindow{};
DWORD g_gameThreadId{};
HHOOK g_gameThreadHook{};
HANDLE g_actionEvent{};
constexpr UINT kAttackSelectedMessage = WM_APP + 0x620;

volatile LONG g_lastActionCode{};
volatile LONG g_actionTokenCounter{};
volatile LONG g_pendingActionToken{};
std::uint32_t g_lastActionTargetId{};
std::uintptr_t g_pendingAttackTarget{};
std::uint32_t g_pendingAttackRequestedId{};

struct Profile {
    std::string schema;
    int profileVersion{};
    std::string architecture;
    std::string timestamp;
    std::string sizeOfImage;
    std::string textHash;
    std::uint32_t expectedTimestamp{};
    std::uint32_t expectedImageSize{};
    std::uintptr_t gameRva{};
    std::size_t localPlayerOffset{};
    std::size_t attackTargetOffset{};
    std::size_t followTargetOffset{};
    std::size_t posX{}, posY{}, posZ{};
    std::size_t creatureIdOffset{};
    std::size_t creatureNameOffset{};
    std::size_t creatureHpPercentOffset{};
    std::uintptr_t monsterVtableRva{};
    std::uintptr_t itemVtableRva{};
    std::size_t itemIdOffset{};
    std::size_t itemCountOffset{};
    bool itemContractReady{};
    std::uintptr_t tileVtableRva{};
    std::size_t tileThingsOffset{};
    std::size_t tileThingsEndOffset{};
    std::size_t tileThingsCapOffset{};
    std::size_t tileThingsElemSize{};
    bool tileThingsRuntimeConfirmed{};
    std::uintptr_t attackMethodRva{};
    bool attackRuntimeConfirmed{};
    bool attackCancelFalseConfirmed{};

    struct Field {
        std::size_t offset{};
        std::string type;
        bool valid{};
    };
    Field health, maxHealth, mana, maxMana;
};

std::optional<Profile> g_profile;

SRWLOCK g_monsterCacheLock = SRWLOCK_INIT;
ULONGLONG g_monsterCacheTick{};
constexpr ULONGLONG kMonsterCacheTtlMs = 1000;

SRWLOCK g_tileCacheLock = SRWLOCK_INIT;
std::vector<std::uintptr_t> g_tileCache;
ULONGLONG g_tileCacheTick{};
std::uint32_t g_tileCacheOriginX{};
std::uint32_t g_tileCacheOriginY{};
std::uint8_t g_tileCacheOriginZ{};
std::uint64_t g_tileCacheGeneration{};
constexpr ULONGLONG kTileCacheMaxAgeMs = 60000;
constexpr ULONGLONG kTileCacheMinRefreshMs = 4000;
constexpr std::uint32_t kTileCacheMoveRefresh = 8;

std::string moduleDirectory();
std::string jsonEscape(const std::string& s);

std::filesystem::path bridgeLogPath() {
    return moduleDirectory() + "\\CromeRamoniaBridge.log";
}

void logLine(const std::string& line) {
    try {
        std::ofstream o(bridgeLogPath(), std::ios::app | std::ios::binary);
        if (o) o << line << "\r\n";
    } catch (...) {}
}

void writeStatusFile() {
    try {
        const auto path = moduleDirectory() + "\\CromeRamoniaBridge.status.txt";
        std::ofstream o(path, std::ios::trunc | std::ios::binary);
        if (!o) return;
        o << "status=" << g_status << "\r\n"
          << "pid=" << GetCurrentProcessId() << "\r\n"
          << "pipe=" << "\\\\.\\pipe\\Crome.Ramonia." << GetCurrentProcessId() << "\r\n"
          << "profileReady=" << (g_profile.has_value() ? "true" : "false") << "\r\n";
        if (!g_profileError.empty()) o << "profileError=" << g_profileError << "\r\n";
    } catch (...) {}
}

std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open profile: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string moduleDirectory() {
    char path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameA(g_module, path, MAX_PATH);
    if (!len) return ".";
    std::string full(path, path + len);
    const auto pos = full.find_last_of("\\/");
    return pos == std::string::npos ? std::string(".") : full.substr(0, pos);
}

std::optional<std::uint64_t> integerValue(const crome::json::Value* v) {
    if (!v || v->isNull()) return std::nullopt;
    if (v->isNumber()) return static_cast<std::uint64_t>(v->number());
    if (!v->isString()) return std::nullopt;
    const auto& s = v->string();
    if (s.empty()) return std::nullopt;
    std::size_t used{};
    const auto n = std::stoull(s, &used, 0);
    if (used != s.size()) return std::nullopt;
    return n;
}

const crome::json::Value* child(const crome::json::Value* v, const char* key) {
    return v && v->isObject() ? v->get(key) : nullptr;
}

std::string stringValue(const crome::json::Value* v, const std::string& fallback = {}) {
    return v && v->isString() ? v->string() : fallback;
}

Profile::Field readField(const crome::json::Value* fields, const char* name) {
    Profile::Field out;
    const auto* f = child(fields, name);
    if (!f || !f->isObject()) return out;
    const auto off = integerValue(f->get("offset"));
    if (!off) return out;
    out.offset = static_cast<std::size_t>(*off);
    out.type = stringValue(f->get("type"));
    out.valid = !out.type.empty();
    return out;
}

Profile loadProfile(const std::string& path) {
    const auto root = crome::json::parse(readText(path));
    if (!root.isObject()) throw std::runtime_error("profile root must be object");

    Profile p;
    p.schema = stringValue(root.get("schema"));

    if (const auto* persistence = root.get("persistence"); persistence && persistence->isObject()) {
        if (const auto n = integerValue(persistence->get("profileVersion")))
            p.profileVersion = static_cast<int>(*n);
    }

    if (const auto* build = root.get("build"); build && build->isObject()) {
        p.architecture = stringValue(build->get("architecture"));
        p.timestamp = stringValue(build->get("timestamp"));
        p.sizeOfImage = stringValue(build->get("sizeOfImage"));
        p.textHash = stringValue(build->get("textHashFNV1A64"));
        if (const auto n = integerValue(build->get("timestamp")))
            p.expectedTimestamp = static_cast<std::uint32_t>(*n);
        if (const auto n = integerValue(build->get("sizeOfImage")))
            p.expectedImageSize = static_cast<std::uint32_t>(*n);
    }

    const auto* owners = root.get("owners");
    const auto game = integerValue(child(owners, "gameRva"));
    if (!game) throw std::runtime_error("owners.gameRva missing");
    p.gameRva = static_cast<std::uintptr_t>(*game);

    const auto* layout = root.get("gameLayout");
    const auto lp = integerValue(child(layout, "localPlayerOffset"));
    if (!lp) throw std::runtime_error("gameLayout.localPlayerOffset missing");
    p.localPlayerOffset = static_cast<std::size_t>(*lp);
    if (const auto n = integerValue(child(layout, "attackTargetOffset")))
        p.attackTargetOffset = static_cast<std::size_t>(*n);
    if (const auto n = integerValue(child(layout, "followTargetOffset")))
        p.followTargetOffset = static_cast<std::size_t>(*n);

    const auto* rtti = root.get("rtti");
    const auto* monster = child(rtti, "Monster");
    const auto* monsterVtables = child(monster, "vtables");
    if (monsterVtables && monsterVtables->isArray() && !monsterVtables->array().empty()) {
        const auto n = integerValue(&monsterVtables->array().front());
        if (n) p.monsterVtableRva = static_cast<std::uintptr_t>(*n);
    }

    const auto* item = child(rtti, "Item");
    const auto* itemVtables = child(item, "vtables");
    if (itemVtables && itemVtables->isArray() && !itemVtables->array().empty()) {
        const auto n = integerValue(&itemVtables->array().front());
        if (n) p.itemVtableRva = static_cast<std::uintptr_t>(*n);
    }

    const auto* actions = root.get("actions");
    const auto* attack = child(actions, "attack");
    if (attack && attack->isObject()) {
        if (const auto n = integerValue(attack->get("methodRva")))
            p.attackMethodRva = static_cast<std::uintptr_t>(*n);
        p.attackRuntimeConfirmed = stringValue(attack->get("status")) == "runtime_confirmed";

        const auto* args = attack->get("args");
        if (args && args->isArray() && args->array().size() >= 2) {
            const auto& cancel = args->array()[1];
            const auto* required = cancel.isObject() ? cancel.get("observedRequiredValue") : nullptr;
            p.attackCancelFalseConfirmed =
                cancel.isObject() &&
                stringValue(cancel.get("semantic")) == "Cancel" &&
                stringValue(cancel.get("abiType")) == "bool" &&
                stringValue(cancel.get("status")) == "runtime_confirmed" &&
                required && required->isBool() && required->boolean() == false;
        }
    }

    const auto* runtime = root.get("runtime");
    const auto* position = child(runtime, "position");
    const auto x = integerValue(child(position, "x"));
    const auto y = integerValue(child(position, "y"));
    const auto z = integerValue(child(position, "z"));
    if (!x || !y || !z) throw std::runtime_error("runtime.position incomplete");
    p.posX = static_cast<std::size_t>(*x);
    p.posY = static_cast<std::size_t>(*y);
    p.posZ = static_cast<std::size_t>(*z);

    const auto* creatureLayout = child(runtime, "creatureLayout");
    const auto* creatureId = child(creatureLayout, "id");
    if (const auto n = integerValue(child(creatureId, "offset")))
        p.creatureIdOffset = static_cast<std::size_t>(*n);
    const auto* creatureName = child(creatureLayout, "name");
    if (const auto n = integerValue(child(creatureName, "offset")))
        p.creatureNameOffset = static_cast<std::size_t>(*n);
    const auto* creatureHp = child(creatureLayout, "hpPercent");
    if (const auto n = integerValue(child(creatureHp, "offset")))
        p.creatureHpPercentOffset = static_cast<std::size_t>(*n);

    const auto* fields = child(runtime, "selectedFields");
    p.health = readField(fields, "health");
    p.maxHealth = readField(fields, "maxHealth");
    p.mana = readField(fields, "mana");
    p.maxMana = readField(fields, "maxMana");

    const auto itemId = readField(fields, "itemId");
    const auto itemCount = readField(fields, "itemCount");
    if (itemId.valid && itemCount.valid &&
        itemId.type == "u16" && itemCount.type == "u16" &&
        p.itemVtableRva) {
        p.itemIdOffset = itemId.offset;
        p.itemCountOffset = itemCount.offset;
        p.itemContractReady = true;
    }

    const auto tileThings = readField(fields, "tileThings");
    const auto tileThingsEnd = readField(fields, "tileThingsEnd");
    const auto tileThingsCap = readField(fields, "tileThingsCap");
    const auto tileThingsElem = readField(fields, "tileThingsElem");
    const auto tileVtable = readField(fields, "tileVtableRva");
    if (tileThings.valid && tileThingsEnd.valid && tileThingsCap.valid &&
        tileThingsElem.valid && tileVtable.valid) {
        p.tileThingsOffset = tileThings.offset;
        p.tileThingsEndOffset = tileThingsEnd.offset;
        p.tileThingsCapOffset = tileThingsCap.offset;
        p.tileThingsElemSize = tileThingsElem.offset;
        p.tileVtableRva = tileVtable.offset;
        p.tileThingsRuntimeConfirmed =
            tileThings.type.find("vector") != std::string::npos &&
            tileThingsElem.type == "bytes" &&
            tileVtable.type == "rva" &&
            p.tileThingsElemSize == 4;
    }
    if (!p.health.valid || !p.maxHealth.valid || !p.mana.valid || !p.maxMana.valid)
        throw std::runtime_error("required player fields missing");
    if (!p.expectedTimestamp || !p.expectedImageSize)
        throw std::runtime_error("build fingerprint incomplete");
    if (!p.monsterVtableRva || !p.creatureIdOffset || !p.creatureNameOffset ||
        !p.creatureHpPercentOffset || !p.attackMethodRva ||
        !p.attackRuntimeConfirmed || !p.attackCancelFalseConfirmed)
        throw std::runtime_error("V37 target/attack contract incomplete");

    return p;
}

std::string findProfilePath() {
    return moduleDirectory() + "\\profiles\\ramonia_x86\\6AB03F94_E281E9CA5BA0AF2.json";
}

template<class T>
bool safeRead(std::uintptr_t address, T& out) {
    SIZE_T got{};
    return ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<LPCVOID>(address), &out, sizeof(out), &got) && got == sizeof(out);
}

bool readNumber(std::uintptr_t address, const std::string& type, double& out) {
    if (type == "double") { double v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    if (type == "float") { float v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    if (type == "u32") { std::uint32_t v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    if (type == "i32") { std::int32_t v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    if (type == "u16") { std::uint16_t v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    if (type == "u8") { std::uint8_t v{}; if (!safeRead(address, v)) return false; out = v; return true; }
    return false;
}

bool isExecutableAddress(std::uintptr_t address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS))
        return false;
    const DWORD p = mbi.Protect & 0xFFu;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

bool readCurrentImageFingerprint() {
    const auto* base = reinterpret_cast<const std::uint8_t*>(g_base);
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    g_imageTimestamp = nt->FileHeader.TimeDateStamp;
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    return true;
}

struct WindowSearch {
    DWORD pid{};
    HWND hwnd{};
};

BOOL CALLBACK enumCurrentProcessWindow(HWND hwnd, LPARAM lp) {
    auto* d = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != d->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;
    d->hwnd = hwnd;
    return FALSE;
}

HWND findCurrentProcessWindow() {
    WindowSearch data{GetCurrentProcessId(), nullptr};
    EnumWindows(&enumCurrentProcessWindow, reinterpret_cast<LPARAM>(&data));
    return data.hwnd;
}

#if defined(__GNUC__)
typedef void (__attribute__((thiscall)) *GameAttackFn)(void*, void*, bool);
#else
typedef void (__thiscall *GameAttackFn)(void*, void*, bool);
#endif

bool targetSnapshot(std::uintptr_t target, std::uint32_t& id) {
    id = 0;
    if (!g_profile || !target) return false;
    std::uintptr_t vtable{};
    if (!safeRead(target, vtable) || vtable != g_base + g_profile->monsterVtableRva)
        return false;
    return safeRead(target + g_profile->creatureIdOffset, id) && id != 0;
}

struct VisibleMonster {
    std::uintptr_t address{};
    std::uint32_t id{};
    std::string name;
    std::uint8_t hpPercent{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint8_t z{};
    std::uint32_t distance{};
};

std::vector<VisibleMonster> g_monsterCache;

std::vector<VisibleMonster> copyMonsterCache() {
    AcquireSRWLockShared(&g_monsterCacheLock);
    auto copy = g_monsterCache;
    ReleaseSRWLockShared(&g_monsterCacheLock);
    return copy;
}

void storeMonsterCache(std::vector<VisibleMonster> monsters) {
    AcquireSRWLockExclusive(&g_monsterCacheLock);
    g_monsterCache = std::move(monsters);
    g_monsterCacheTick = GetTickCount64();
    ReleaseSRWLockExclusive(&g_monsterCacheLock);
}

std::optional<std::uintptr_t> cachedMonsterAddressById(std::uint32_t id) {
    if (!id) return std::nullopt;
    AcquireSRWLockShared(&g_monsterCacheLock);
    std::optional<std::uintptr_t> result;
    for (const auto& m : g_monsterCache) {
        if (m.id == id) { result = m.address; break; }
    }
    ReleaseSRWLockShared(&g_monsterCacheLock);
    return result;
}

bool monsterCacheFresh() {
    AcquireSRWLockShared(&g_monsterCacheLock);
    const auto tick = g_monsterCacheTick;
    ReleaseSRWLockShared(&g_monsterCacheLock);
    return tick && (GetTickCount64() - tick) < kMonsterCacheTtlMs;
}

std::string safeReadInlineString(std::uintptr_t address, std::size_t maxLen = 64) {
    std::string out;
    out.reserve(maxLen);
    for (std::size_t i = 0; i < maxLen; ++i) {
        char c{};
        if (!safeRead(address + i, c)) return {};
        if (c == '\0') break;
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc > 0x7E) return {};
        out.push_back(c);
    }
    return out;
}

bool readableProtection(DWORD protect) {
    if (protect & PAGE_GUARD) return false;
    if ((protect & 0xFFu) == PAGE_NOACCESS) return false;
    const DWORD p = protect & 0xFFu;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
           p == PAGE_EXECUTE_WRITECOPY;
}

bool localPlayerPosition(std::uint32_t& x, std::uint32_t& y, std::uint8_t& z) {
    x = y = 0; z = 0;
    if (!g_profile) return false;
    const auto game = g_base + g_profile->gameRva;
    std::uintptr_t player{};
    if (!safeRead(game + g_profile->localPlayerOffset, player) || !player) return false;
    return safeRead(player + g_profile->posX, x) &&
           safeRead(player + g_profile->posY, y) &&
           safeRead(player + g_profile->posZ, z) &&
           x > 0 && y > 0 && z <= 15;
}

std::vector<VisibleMonster> scanVisibleMonsters(std::uint32_t maxRange = 15) {
    std::vector<VisibleMonster> out;
    if (!g_profile) return out;

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz)) return out;

    const std::uint32_t wantedVtable =
        static_cast<std::uint32_t>(g_base + g_profile->monsterVtableRva);

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto address = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const auto maxAddress = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    constexpr SIZE_T kChunk = 1u << 20;
    std::vector<std::uint8_t> buffer(kChunk);

    while (address < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) break;

        const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEnd = regionBase + mbi.RegionSize;
        if (regionEnd <= address) break;

        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            readableProtection(mbi.Protect)) {
            for (auto chunkBase = regionBase; chunkBase < regionEnd; ) {
                const SIZE_T want = static_cast<SIZE_T>(
                    std::min<std::uintptr_t>(kChunk, regionEnd - chunkBase));
                SIZE_T got{};
                if (ReadProcessMemory(GetCurrentProcess(),
                                      reinterpret_cast<LPCVOID>(chunkBase),
                                      buffer.data(), want, &got) && got >= sizeof(std::uint32_t)) {
                    for (SIZE_T i = 0; i + sizeof(std::uint32_t) <= got; i += 4) {
                        std::uint32_t vt{};
                        std::memcpy(&vt, buffer.data() + i, sizeof(vt));
                        if (vt != wantedVtable) continue;

                        const auto candidate = chunkBase + i;
                        std::uint32_t id{}, x{}, y{};
                        std::uint8_t z{}, hpPercent{};
                        if (!safeRead(candidate + g_profile->creatureIdOffset, id) || !id ||
                            !safeRead(candidate + g_profile->posX, x) ||
                            !safeRead(candidate + g_profile->posY, y) ||
                            !safeRead(candidate + g_profile->posZ, z) ||
                            !safeRead(candidate + g_profile->creatureHpPercentOffset, hpPercent)) {
                            continue;
                        }

                        const auto name = safeReadInlineString(
                            candidate + g_profile->creatureNameOffset, 48);

                        if (!x || !y || z != pz || hpPercent > 100 || name.empty()) continue;
                        const auto dx = x > px ? x - px : px - x;
                        const auto dy = y > py ? y - py : py - y;
                        const auto dist = std::max(dx, dy);
                        if (dist > maxRange) continue;

                        bool duplicate = false;
                        for (const auto& m : out) {
                            if (m.address == candidate) { duplicate = true; break; }
                        }
                        if (!duplicate)
                            out.push_back({candidate, id, name, hpPercent, x, y, z, dist});
                    }
                }
                chunkBase += want ? want : 1;
            }
        }

        address = regionEnd;
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.id < b.id;
    });
    return out;
}


struct TileMonsterDiagnostic {
    VisibleMonster monster;
    std::uintptr_t tileAddress{};
    std::uint32_t tileThingCount{};
    bool tileContainsLocalPlayer{};
};

bool readTileVectorHeader(std::uintptr_t tile,
                          std::uint32_t& begin,
                          std::uint32_t& end,
                          std::uint32_t& cap,
                          std::uint32_t& count) {
    begin = end = cap = count = 0;
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed || !tile) return false;

    const auto& p = *g_profile;
    std::uintptr_t vt{};
    if (!safeRead(tile, vt) || vt != g_base + p.tileVtableRva) return false;

    if (!safeRead(tile + p.tileThingsOffset, begin) ||
        !safeRead(tile + p.tileThingsEndOffset, end) ||
        !safeRead(tile + p.tileThingsCapOffset, cap))
        return false;

    if (p.tileThingsElemSize != 4) return false;

    // A default-constructed empty std::vector is allowed to be all zeros.
    if (!begin && !end && !cap) {
        count = 0;
        return true;
    }

    if (!begin || end < begin || cap < end) return false;
    const std::uint32_t bytes = end - begin;
    const std::uint32_t capBytes = cap - begin;
    if (bytes % 4 || capBytes % 4) return false;

    count = bytes / 4;
    const std::uint32_t capacity = capBytes / 4;
    return count <= 80 && capacity >= count && capacity <= 160;
}

bool readStableTileThings(std::uintptr_t tile,
                          std::vector<std::uintptr_t>& things,
                          bool& containsLocalPlayer) {
    things.clear();
    containsLocalPlayer = false;
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed || !tile) return false;

    const auto& p = *g_profile;
    std::uint32_t begin1{}, end1{}, cap1{}, count1{};
    if (!readTileVectorHeader(tile, begin1, end1, cap1, count1)) return false;
    if (count1 == 0) return true;

    std::vector<std::uint32_t> raw(count1);
    SIZE_T got{};
    if (!ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(begin1)),
                           raw.data(), raw.size() * sizeof(std::uint32_t), &got) ||
        got != raw.size() * sizeof(std::uint32_t))
        return false;

    // Reject the snapshot if the vector changed while its pointer array was copied.
    std::uint32_t begin2{}, end2{}, cap2{}, count2{};
    if (!readTileVectorHeader(tile, begin2, end2, cap2, count2) ||
        begin1 != begin2 || end1 != end2 || cap1 != cap2 || count1 != count2)
        return false;

    std::uintptr_t localPlayer{};
    const auto game = g_base + p.gameRva;
    safeRead(game + p.localPlayerOffset, localPlayer);

    things.reserve(count1);
    for (const auto rawPtr : raw) {
        const auto thing = static_cast<std::uintptr_t>(rawPtr);
        if (!thing) return false;

        std::uintptr_t thingVtable{};
        if (!safeRead(thing, thingVtable) ||
            thingVtable < g_base || thingVtable >= g_base + g_imageSize)
            return false;

        if (localPlayer && thing == localPlayer) containsLocalPlayer = true;
        things.push_back(thing);
    }
    return true;
}

std::vector<std::uintptr_t> discoverTileAddresses() {
    std::vector<std::uintptr_t> tiles;
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed) return tiles;

    const std::uint32_t wantedTileVtable =
        static_cast<std::uint32_t>(g_base + g_profile->tileVtableRva);

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto address = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const auto maxAddress = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    constexpr SIZE_T kChunk = 1u << 20;
    std::vector<std::uint8_t> buffer(kChunk);

    while (address < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) break;

        const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEnd = regionBase + mbi.RegionSize;
        if (regionEnd <= address) break;

        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            readableProtection(mbi.Protect)) {
            for (auto chunkBase = regionBase; chunkBase < regionEnd; ) {
                const SIZE_T want = static_cast<SIZE_T>(
                    std::min<std::uintptr_t>(kChunk, regionEnd - chunkBase));
                SIZE_T got{};
                if (ReadProcessMemory(GetCurrentProcess(),
                                      reinterpret_cast<LPCVOID>(chunkBase),
                                      buffer.data(), want, &got) &&
                    got >= sizeof(std::uint32_t)) {
                    for (SIZE_T i = 0; i + sizeof(std::uint32_t) <= got; i += 4) {
                        std::uint32_t vt{};
                        std::memcpy(&vt, buffer.data() + i, sizeof(vt));
                        if (vt != wantedTileVtable) continue;

                        const auto tile = chunkBase + i;
                        std::uint32_t begin{}, finish{}, cap{}, count{};
                        if (!readTileVectorHeader(tile, begin, finish, cap, count))
                            continue;
                        tiles.push_back(tile);
                    }
                }
                chunkBase += want ? want : 1;
            }
        }
        address = regionEnd;
    }

    std::sort(tiles.begin(), tiles.end());
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    return tiles;
}

std::vector<std::uintptr_t> copyTileCache() {
    AcquireSRWLockShared(&g_tileCacheLock);
    auto copy = g_tileCache;
    ReleaseSRWLockShared(&g_tileCacheLock);
    return copy;
}

struct TileCacheInfo {
    std::size_t count{};
    ULONGLONG ageMs{};
    std::uint32_t originX{};
    std::uint32_t originY{};
    std::uint8_t originZ{};
    std::uint64_t generation{};
};

TileCacheInfo tileCacheInfo() {
    TileCacheInfo info;
    AcquireSRWLockShared(&g_tileCacheLock);
    info.count = g_tileCache.size();
    info.originX = g_tileCacheOriginX;
    info.originY = g_tileCacheOriginY;
    info.originZ = g_tileCacheOriginZ;
    info.generation = g_tileCacheGeneration;
    const auto tick = g_tileCacheTick;
    ReleaseSRWLockShared(&g_tileCacheLock);
    info.ageMs = tick ? GetTickCount64() - tick : 0;
    return info;
}

bool refreshTileCache(std::uint32_t px, std::uint32_t py, std::uint8_t pz) {
    const auto discovered = discoverTileAddresses();
    if (discovered.empty()) return false;

    AcquireSRWLockExclusive(&g_tileCacheLock);
    g_tileCache = discovered;
    g_tileCacheTick = GetTickCount64();
    g_tileCacheOriginX = px;
    g_tileCacheOriginY = py;
    g_tileCacheOriginZ = pz;
    ++g_tileCacheGeneration;
    ReleaseSRWLockExclusive(&g_tileCacheLock);

    logLine("[TILE CACHE] refreshed tiles=" + std::to_string(discovered.size()) +
            " origin=" + std::to_string(px) + "," + std::to_string(py) + "," +
            std::to_string(static_cast<unsigned>(pz)));
    return true;
}

bool ensureTileCache(std::uint32_t px, std::uint32_t py, std::uint8_t pz,
                     bool force = false) {
    const auto info = tileCacheInfo();
    const auto dx = px > info.originX ? px - info.originX : info.originX - px;
    const auto dy = py > info.originY ? py - info.originY : info.originY - py;
    const auto moved = std::max(dx, dy);

    bool refresh = force || info.count == 0;
    if (!refresh && info.ageMs >= kTileCacheMaxAgeMs) refresh = true;
    if (!refresh && info.originZ != pz && info.ageMs >= 1000) refresh = true;
    if (!refresh && moved >= kTileCacheMoveRefresh &&
        info.ageMs >= kTileCacheMinRefreshMs)
        refresh = true;

    return !refresh || refreshTileCache(px, py, pz);
}

bool snapshotMonsterFromThing(std::uintptr_t thing,
                              std::uint32_t px, std::uint32_t py, std::uint8_t pz,
                              std::uint32_t maxRange,
                              VisibleMonster& out) {
    out = {};
    if (!g_profile || !thing) return false;

    std::uintptr_t thingVtable{};
    if (!safeRead(thing, thingVtable) ||
        thingVtable != g_base + g_profile->monsterVtableRva)
        return false;

    std::uint32_t id{}, x{}, y{};
    std::uint8_t z{}, hpPercent{};
    if (!safeRead(thing + g_profile->creatureIdOffset, id) || !id ||
        !safeRead(thing + g_profile->posX, x) ||
        !safeRead(thing + g_profile->posY, y) ||
        !safeRead(thing + g_profile->posZ, z) ||
        !safeRead(thing + g_profile->creatureHpPercentOffset, hpPercent))
        return false;

    if (!x || !y || z != pz || hpPercent > 100) return false;
    const auto name = safeReadInlineString(
        thing + g_profile->creatureNameOffset, 48);
    if (name.empty()) return false;

    const auto dx = x > px ? x - px : px - x;
    const auto dy = y > py ? y - py : py - y;
    const auto dist = std::max(dx, dy);
    if (dist > maxRange) return false;

    out = {thing, id, name, hpPercent, x, y, z, dist};
    return true;
}

std::vector<TileMonsterDiagnostic> scanCachedTileMonsters(
    std::uint32_t maxRange = 15,
    bool forceTileRefresh = false) {
    std::vector<TileMonsterDiagnostic> out;
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed) return out;

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz)) return out;
    if (!ensureTileCache(px, py, pz, forceTileRefresh)) return out;

    const auto tiles = copyTileCache();
    for (const auto tile : tiles) {
        std::vector<std::uintptr_t> things;
        bool containsLocalPlayer = false;
        if (!readStableTileThings(tile, things, containsLocalPlayer))
            continue;

        for (const auto thing : things) {
            VisibleMonster monster;
            if (!snapshotMonsterFromThing(thing, px, py, pz, maxRange, monster))
                continue;

            bool duplicate = false;
            for (const auto& existing : out) {
                if (existing.monster.address == monster.address ||
                    existing.monster.id == monster.id) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            TileMonsterDiagnostic d;
            d.monster = std::move(monster);
            d.tileAddress = tile;
            d.tileThingCount = static_cast<std::uint32_t>(things.size());
            d.tileContainsLocalPlayer = containsLocalPlayer;
            out.push_back(std::move(d));
        }
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.monster.distance != b.monster.distance)
            return a.monster.distance < b.monster.distance;
        return a.monster.id < b.monster.id;
    });
    return out;
}

std::uintptr_t resolveTileMonsterById(std::uint32_t id) {
    if (!id || !g_profile || !g_profile->tileThingsRuntimeConfirmed) return 0;

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz)) return 0;

    // Never perform a broad discovery scan from the game thread.
    // ATTACK_ID can use only the current Tile cache and revalidates the Thing
    // membership immediately before Game::attack.
    const auto tiles = copyTileCache();
    for (const auto tile : tiles) {
        std::vector<std::uintptr_t> things;
        bool containsLocalPlayer = false;
        if (!readStableTileThings(tile, things, containsLocalPlayer))
            continue;

        for (const auto thing : things) {
            VisibleMonster monster;
            if (!snapshotMonsterFromThing(thing, px, py, pz, 15, monster))
                continue;
            if (monster.id == id) return thing;
        }
    }
    return 0;
}

std::vector<TileMonsterDiagnostic> scanTileMonstersReadOnly(std::uint32_t maxRange = 15) {
    return scanCachedTileMonsters(maxRange);
}

struct TileItemDiagnostic {
    std::uintptr_t address{};
    std::uintptr_t tileAddress{};
    std::uint16_t id{};
    std::uint16_t count{};
    std::uint32_t tileSlot{};
    std::uint32_t stackIndex{};
    std::uint32_t tileThingCount{};
};

struct LootBaselineState {
    bool valid{};
    std::uint64_t tileGeneration{};
    std::vector<TileItemDiagnostic> items;
    std::vector<TileMonsterDiagnostic> monsters;
};

SRWLOCK g_lootBaselineLock = SRWLOCK_INIT;
LootBaselineState g_lootBaseline;

std::uint32_t tileSlotOf(const std::vector<std::uintptr_t>& tiles,
                         std::uintptr_t tile) {
    for (std::size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i] == tile) return static_cast<std::uint32_t>(i);
    return 0xFFFFFFFFu;
}

std::vector<TileItemDiagnostic> scanCachedTileItems() {
    std::vector<TileItemDiagnostic> out;
    if (!g_profile || !g_profile->itemContractReady ||
        !g_profile->tileThingsRuntimeConfirmed)
        return out;

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz)) return out;
    if (!ensureTileCache(px, py, pz)) return out;

    const auto tiles = copyTileCache();
    const auto wantedItemVtable = g_base + g_profile->itemVtableRva;

    for (std::size_t tileIndex = 0; tileIndex < tiles.size(); ++tileIndex) {
        const auto tile = tiles[tileIndex];
        std::vector<std::uintptr_t> things;
        bool containsLocalPlayer = false;
        if (!readStableTileThings(tile, things, containsLocalPlayer))
            continue;

        for (std::size_t stackIndex = 0; stackIndex < things.size(); ++stackIndex) {
            const auto thing = things[stackIndex];
            std::uintptr_t vt{};
            if (!safeRead(thing, vt) || vt != wantedItemVtable)
                continue;

            std::uint16_t id{}, count{};
            if (!safeRead(thing + g_profile->itemIdOffset, id) || !id ||
                !safeRead(thing + g_profile->itemCountOffset, count))
                continue;

            TileItemDiagnostic d;
            d.address = thing;
            d.tileAddress = tile;
            d.id = id;
            d.count = count;
            d.tileSlot = static_cast<std::uint32_t>(tileIndex);
            d.stackIndex = static_cast<std::uint32_t>(stackIndex);
            d.tileThingCount = static_cast<std::uint32_t>(things.size());
            out.push_back(d);
        }
    }
    return out;
}

bool sameItemIdentity(const TileItemDiagnostic& a, const TileItemDiagnostic& b) {
    return a.address == b.address && a.id == b.id;
}

bool currentMonsterContainsId(const std::vector<TileMonsterDiagnostic>& current,
                              std::uint32_t id) {
    for (const auto& m : current)
        if (m.monster.id == id) return true;
    return false;
}

std::string lootMarkResponse() {
    if (!g_profile || !g_profile->itemContractReady)
        return R"({"ok":false,"cmd":"LOOT_MARK","error":"item_contract_not_ready"})";

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz))
        return R"({"ok":false,"cmd":"LOOT_MARK","error":"player_unavailable"})";

    if (!ensureTileCache(px, py, pz))
        return R"({"ok":false,"cmd":"LOOT_MARK","error":"tile_cache_unavailable"})";

    const auto items = scanCachedTileItems();
    const auto monsters = scanCachedTileMonsters(15);
    const auto cache = tileCacheInfo();
    const auto tiles = copyTileCache();

    AcquireSRWLockExclusive(&g_lootBaselineLock);
    g_lootBaseline.valid = true;
    g_lootBaseline.tileGeneration = cache.generation;
    g_lootBaseline.items = items;
    g_lootBaseline.monsters = monsters;
    ReleaseSRWLockExclusive(&g_lootBaselineLock);

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"LOOT_MARK\",\"readOnly\":true,"
      << "\"tileGeneration\":" << cache.generation
      << ",\"baselineItems\":" << items.size()
      << ",\"trackedMonsters\":[";

    for (std::size_t i = 0; i < monsters.size() && i < 32; ++i) {
        if (i) o << ",";
        const auto& m = monsters[i];
        o << "{\"id\":" << m.monster.id
          << ",\"name\":\"" << jsonEscape(m.monster.name) << "\""
          << ",\"hpPercent\":" << static_cast<unsigned>(m.monster.hpPercent)
          << ",\"distance\":" << m.monster.distance
          << ",\"tileSlot\":" << tileSlotOf(tiles, m.tileAddress)
          << "}";
    }
    o << "]}";
    return o.str();
}

std::string lootDiffResponse() {
    LootBaselineState baseline;
    AcquireSRWLockShared(&g_lootBaselineLock);
    baseline = g_lootBaseline;
    ReleaseSRWLockShared(&g_lootBaselineLock);

    if (!baseline.valid)
        return R"({"ok":false,"cmd":"LOOT_DIFF","error":"no_baseline","hint":"run_LOOT_MARK_first"})";

    const auto cache = tileCacheInfo();
    if (cache.generation != baseline.tileGeneration) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"LOOT_DIFF\","
          << "\"error\":\"tile_cache_changed_recapture\","
          << "\"baselineGeneration\":" << baseline.tileGeneration
          << ",\"currentGeneration\":" << cache.generation << "}";
        return o.str();
    }

    const auto items = scanCachedTileItems();
    const auto monsters = scanCachedTileMonsters(15);
    const auto tiles = copyTileCache();

    std::vector<TileItemDiagnostic> added;
    for (const auto& now : items) {
        bool existed = false;
        for (const auto& old : baseline.items) {
            if (sameItemIdentity(now, old)) { existed = true; break; }
        }
        if (!existed) added.push_back(now);
    }

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"LOOT_DIFF\",\"readOnly\":true,"
      << "\"tileGeneration\":" << cache.generation
      << ",\"addedCount\":" << added.size()
      << ",\"addedItems\":[";

    for (std::size_t i = 0; i < added.size() && i < 64; ++i) {
        if (i) o << ",";
        const auto& item = added[i];

        o << "{\"id\":" << item.id
          << ",\"count\":" << item.count
          << ",\"tileSlot\":" << item.tileSlot
          << ",\"stackIndex\":" << item.stackIndex
          << ",\"tileThingCount\":" << item.tileThingCount
          << ",\"baselineHadMonster\":";

        bool hadMonster = false;
        for (const auto& oldMonster : baseline.monsters)
            if (oldMonster.tileAddress == item.tileAddress) {
                hadMonster = true;
                break;
            }
        o << (hadMonster ? "true" : "false")
          << ",\"baselineMonsters\":[";

        bool firstMonster = true;
        for (const auto& oldMonster : baseline.monsters) {
            if (oldMonster.tileAddress != item.tileAddress) continue;
            if (!firstMonster) o << ",";
            firstMonster = false;
            o << "{\"id\":" << oldMonster.monster.id
              << ",\"name\":\"" << jsonEscape(oldMonster.monster.name) << "\""
              << "}";
        }
        o << "]}";
    }

    o << "],\"removedMonsters\":[";
    bool firstRemoved = true;
    for (const auto& oldMonster : baseline.monsters) {
        if (currentMonsterContainsId(monsters, oldMonster.monster.id))
            continue;
        if (!firstRemoved) o << ",";
        firstRemoved = false;
        o << "{\"id\":" << oldMonster.monster.id
          << ",\"name\":\"" << jsonEscape(oldMonster.monster.name) << "\""
          << ",\"tileSlot\":" << tileSlotOf(tiles, oldMonster.tileAddress)
          << "}";
    }
    o << "]}";
    return o.str();
}

std::string tileMonstersResponse() {
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed)
        return R"({"ok":false,"cmd":"GET_TILE_MONSTERS","error":"tile_contract_not_ready"})";

    const auto monsters = scanCachedTileMonsters(15);
    const auto cache = tileCacheInfo();

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_TILE_MONSTERS\","
      << "\"source\":\"Tile::things-cache\",\"readOnly\":true,"
      << "\"cachedTiles\":" << cache.count
      << ",\"cacheAgeMs\":" << cache.ageMs
      << ",\"count\":" << monsters.size() << ",\"monsters\":[";

    for (std::size_t i = 0; i < monsters.size() && i < 64; ++i) {
        if (i) o << ",";
        const auto& d = monsters[i];
        const auto& m = d.monster;
        o << "{\"id\":" << m.id
          << ",\"name\":\"" << jsonEscape(m.name) << "\""
          << ",\"hpPercent\":" << static_cast<unsigned>(m.hpPercent)
          << ",\"x\":" << m.x
          << ",\"y\":" << m.y
          << ",\"z\":" << static_cast<unsigned>(m.z)
          << ",\"distance\":" << m.distance
          << ",\"tileThingCount\":" << d.tileThingCount
          << ",\"tileContainsLocalPlayer\":"
          << (d.tileContainsLocalPlayer ? "true" : "false")
          << "}";
    }
    o << "]}";
    return o.str();
}

std::uintptr_t resolveVisibleMonsterById(std::uint32_t id) {
    return resolveTileMonsterById(id);
}

bool executeAttackSelectedOnGameThread() {
    InterlockedExchange(&g_lastActionCode, 0);
    g_lastActionTargetId = 0;

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode, 5);
        return false;
    }

    const auto& p = *g_profile;
    const auto game = g_base + p.gameRva;

    std::uintptr_t target = 0;

    // ATTACK_ID carries only the semantic creature ID across threads.
    // Resolve Monster* from the CURRENT cached Tile::things membership on the
    // actual game/window thread immediately before validation + Game::attack.
    // No Monster pointer discovered by the old broad heap scan is used here.
    if (g_pendingAttackRequestedId) {
        target = resolveVisibleMonsterById(g_pendingAttackRequestedId);
        if (!target) {
            InterlockedExchange(&g_lastActionCode, 10);
            return false;
        }
    } else {
        target = g_pendingAttackTarget;
        if (!target) {
            std::uintptr_t followTarget{};
            std::uintptr_t attackTarget{};
            safeRead(game + p.followTargetOffset, followTarget);
            safeRead(game + p.attackTargetOffset, attackTarget);
            target = followTarget ? followTarget : attackTarget;
        }
    }

    if (!target) {
        InterlockedExchange(&g_lastActionCode, 2);
        return false;
    }

    std::uint32_t id{};
    if (!targetSnapshot(target, id)) {
        InterlockedExchange(&g_lastActionCode, 3);
        return false;
    }
    if (g_pendingAttackRequestedId && id != g_pendingAttackRequestedId) {
        InterlockedExchange(&g_lastActionCode, 9);
        return false;
    }

    const auto functionAddress = g_base + p.attackMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode, 4);
        return false;
    }

    g_lastActionTargetId = id;
    const auto fn = reinterpret_cast<GameAttackFn>(functionAddress);

    logLine("[ACTION] ATTACK_SELECTED game-thread id=" + std::to_string(id));
    // V4 ABI experiment: V37 captured Creature* as arg1, but the
    // original probe only inspected one attack argument. OTC-family Game::attack
    // commonly has a trailing bool cancel; false is the path that transmits the
    // attack rather than only updating local target state.
    fn(reinterpret_cast<void*>(game), reinterpret_cast<void*>(target), false);

    std::uintptr_t after{};
    safeRead(game + p.attackTargetOffset, after);
    if (after == target) {
        InterlockedExchange(&g_lastActionCode, 1);
        logLine("[ACTION] ATTACK_SELECTED success id=" + std::to_string(id));
        return true;
    }

    InterlockedExchange(&g_lastActionCode, 6);
    logLine("[ACTION] ATTACK_SELECTED returned but AttackTarget did not match");
    return false;
}

LRESULT CALLBACK gameThreadGetMessageHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && lParam) {
        auto* msg = reinterpret_cast<MSG*>(lParam);
        if (msg->message == kAttackSelectedMessage) {
            const LONG token = static_cast<LONG>(msg->wParam);
            const LONG expected = InterlockedCompareExchange(&g_pendingActionToken, 0, 0);
            if (token != 0 && token == expected) {
                executeAttackSelectedOnGameThread();

                // Preserve the normal hook chain first, then remove our hook
                // from inside the target thread itself. No permanent hook is
                // left installed after the action message is consumed.
                const HHOOK hook = g_gameThreadHook;
                const LRESULT next = CallNextHookEx(hook, code, wParam, lParam);
                if (hook) {
                    UnhookWindowsHookEx(hook);
                    if (g_gameThreadHook == hook) g_gameThreadHook = nullptr;
                }
                if (g_actionEvent) SetEvent(g_actionEvent);
                return next;
            }
        }
    }
    return CallNextHookEx(g_gameThreadHook, code, wParam, lParam);
}
bool dispatcherReady() {
    return g_gameWindow && IsWindow(g_gameWindow) && g_gameThreadId != 0;
}

bool prepareGameThreadDispatcher() {
    if (dispatcherReady()) return true;

    g_gameWindow = findCurrentProcessWindow();
    if (!g_gameWindow) {
        logLine("[DISPATCH] main game window not found");
        return false;
    }

    g_gameThreadId = GetWindowThreadProcessId(g_gameWindow, nullptr);
    if (!g_gameThreadId) {
        logLine("[DISPATCH] game window thread id unavailable");
        g_gameWindow = nullptr;
        return false;
    }

    logLine("[DISPATCH] on-demand game-thread target ready tid=" +
            std::to_string(g_gameThreadId));
    return true;
}

void cleanupOnDemandDispatcher() {
    if (g_gameThreadHook) {
        const HHOOK hook = g_gameThreadHook;
        g_gameThreadHook = nullptr;
        UnhookWindowsHookEx(hook);
    }
    if (g_actionEvent) {
        const HANDLE event = g_actionEvent;
        g_actionEvent = nullptr;
        CloseHandle(event);
    }
}

bool dispatchAttackToGameThread(DWORD& win32Error) {
    win32Error = ERROR_SUCCESS;

    if (!prepareGameThreadDispatcher()) {
        win32Error = ERROR_INVALID_WINDOW_HANDLE;
        return false;
    }

    // V12: no hook/event remains installed while the bridge is idle.
    cleanupOnDemandDispatcher();

    g_actionEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_actionEvent) {
        win32Error = GetLastError();
        return false;
    }

    SetLastError(0);
    g_gameThreadHook = SetWindowsHookExW(
        WH_GETMESSAGE,
        &gameThreadGetMessageHook,
        nullptr,
        g_gameThreadId);
    if (!g_gameThreadHook) {
        win32Error = GetLastError();
        cleanupOnDemandDispatcher();
        return false;
    }

    ResetEvent(g_actionEvent);
    InterlockedExchange(&g_lastActionCode, 0);

    LONG token = InterlockedIncrement(&g_actionTokenCounter);
    if (token == 0) token = InterlockedIncrement(&g_actionTokenCounter);
    InterlockedExchange(&g_pendingActionToken, token);

    SetLastError(0);
    if (!PostThreadMessageW(
            g_gameThreadId,
            kAttackSelectedMessage,
            static_cast<WPARAM>(static_cast<ULONG_PTR>(token)),
            0)) {
        win32Error = GetLastError();
        InterlockedCompareExchange(&g_pendingActionToken, 0, token);
        cleanupOnDemandDispatcher();
        return false;
    }

    const DWORD wait = WaitForSingleObject(g_actionEvent, 3000);
    if (wait != WAIT_OBJECT_0) {
        win32Error = (wait == WAIT_TIMEOUT) ? ERROR_TIMEOUT : GetLastError();
        InterlockedCompareExchange(&g_pendingActionToken, 0, token);
        cleanupOnDemandDispatcher();
        return false;
    }

    InterlockedCompareExchange(&g_pendingActionToken, 0, token);

    // The hook normally removes itself in the callback. This is only a safety
    // cleanup for unexpected paths.
    cleanupOnDemandDispatcher();
    return true;
}

std::string actionCodeText(LONG code) {
    switch (code) {
        case 1: return "success";
        case 2: return "no_selected_target";
        case 3: return "selected_target_is_not_confirmed_monster";
        case 4: return "attack_function_not_executable";
        case 5: return "build_not_validated";
        case 6: return "attack_target_not_updated";
        case 7: return "dispatcher_not_ready";
        case 8: return "dispatcher_timeout";
        case 9: return "creature_id_mismatch";
        case 10: return "creature_id_not_visible";
        default: return "unknown_action_result";
    }
}

std::string jsonEscape(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o << "\\\\"; break;
            case '"': o << "\\\""; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (c < 0x20) o << '?';
                else o << static_cast<char>(c);
        }
    }
    return o.str();
}

std::optional<std::uint32_t> uint32FieldFromRequest(
    const std::string& request, const char* field) {
    try {
        const auto root = crome::json::parse(request);
        if (!root.isObject()) return std::nullopt;
        const auto n = integerValue(root.get(field));
        if (!n || *n == 0 || *n > 0xFFFFFFFFull) return std::nullopt;
        return static_cast<std::uint32_t>(*n);
    } catch (...) {
        return std::nullopt;
    }
}

std::string commandFromRequest(const std::string& request) {
    try {
        const auto root = crome::json::parse(request);
        if (root.isObject()) {
            if (const auto* v = root.get("cmd"); v && v->isString()) return v->string();
        }
    } catch (...) {}
    std::string s = request;
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){ return std::isspace(c); }), s.end());
    return s;
}

std::string pipeName() {
    return "\\\\.\\pipe\\Crome.Ramonia." + std::to_string(GetCurrentProcessId());
}

std::string playerResponse(const Profile& p, std::uintptr_t base) {
    const auto game = base + p.gameRva;
    std::uintptr_t localPlayer{};
    if (!safeRead(game + p.localPlayerOffset, localPlayer) || !localPlayer)
        return R"({"ok":false,"cmd":"GET_PLAYER","error":"local_player_unavailable"})";

    std::uint32_t x{}, y{};
    std::uint8_t z{};
    double hp{}, maxHp{}, mana{}, maxMana{};
    const bool ok =
        safeRead(localPlayer + p.posX, x) &&
        safeRead(localPlayer + p.posY, y) &&
        safeRead(localPlayer + p.posZ, z) &&
        readNumber(localPlayer + p.health.offset, p.health.type, hp) &&
        readNumber(localPlayer + p.maxHealth.offset, p.maxHealth.type, maxHp) &&
        readNumber(localPlayer + p.mana.offset, p.mana.type, mana) &&
        readNumber(localPlayer + p.maxMana.offset, p.maxMana.type, maxMana);

    if (!ok) return R"({"ok":false,"cmd":"GET_PLAYER","error":"snapshot_read_failed"})";

    std::ostringstream o;
    o << std::fixed << std::setprecision(2)
      << "{\"ok\":true,\"cmd\":\"GET_PLAYER\",\"player\":{"
      << "\"health\":" << hp << ",\"maxHealth\":" << maxHp
      << ",\"mana\":" << mana << ",\"maxMana\":" << maxMana
      << ",\"position\":{\"x\":" << x << ",\"y\":" << y << ",\"z\":"
      << static_cast<unsigned>(z) << "}}}";
    return o.str();
}

std::string combatTargetsResponse(const Profile& p) {
    const auto game = g_base + p.gameRva;
    std::uintptr_t attackTarget{};
    std::uintptr_t followTarget{};
    safeRead(game + p.attackTargetOffset, attackTarget);
    safeRead(game + p.followTargetOffset, followTarget);

    std::uint32_t attackId{};
    std::uint32_t followId{};
    const bool attackMonster = targetSnapshot(attackTarget, attackId);
    const bool followMonster = targetSnapshot(followTarget, followId);

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_COMBAT_TARGETS\","
      << "\"attack\":{\"present\":" << (attackTarget ? "true" : "false")
      << ",\"monster\":" << (attackMonster ? "true" : "false")
      << ",\"id\":" << attackId << "},"
      << "\"follow\":{\"present\":" << (followTarget ? "true" : "false")
      << ",\"monster\":" << (followMonster ? "true" : "false")
      << ",\"id\":" << followId << "}}";
    return o.str();
}

std::string visibleMonstersResponse() {
    const auto tileMonsters = scanCachedTileMonsters(15);
    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_VISIBLE_MONSTERS\","
      << "\"source\":\"Tile::things-cache\",\"count\":" << tileMonsters.size()
      << ",\"monsters\":[";
    for (std::size_t i = 0; i < tileMonsters.size() && i < 32; ++i) {
        if (i) o << ",";
        const auto& m = tileMonsters[i].monster;
        o << "{\"id\":" << m.id
          << ",\"name\":\"" << jsonEscape(m.name) << "\""
          << ",\"hpPercent\":" << static_cast<unsigned>(m.hpPercent)
          << ",\"x\":" << m.x
          << ",\"y\":" << m.y
          << ",\"z\":" << static_cast<unsigned>(m.z)
          << ",\"distance\":" << m.distance << "}";
    }
    o << "]}";
    return o.str();
}

std::string attackIdResponse(std::uint32_t creatureId) {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"ATTACK_ID","error":"build_not_validated"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"ATTACK_ID","error":"dispatcher_not_ready"})";

    const auto tileVisible = scanCachedTileMonsters(15);
    const bool idVisible = std::any_of(
        tileVisible.begin(), tileVisible.end(),
        [creatureId](const auto& d){ return d.monster.id == creatureId; });
    if (!idVisible)
        return R"({"ok":false,"cmd":"ATTACK_ID","error":"creature_id_not_visible"})";

    g_lastActionTargetId = 0;
    g_pendingAttackTarget = 0;
    g_pendingAttackRequestedId = creatureId;

    DWORD win32{};
    const bool dispatched = dispatchAttackToGameThread(win32);

    g_pendingAttackTarget = 0;
    g_pendingAttackRequestedId = 0;

    if (!dispatched) {
        InterlockedExchange(&g_lastActionCode, 8);
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"ATTACK_ID\",\"error\":\"dispatcher_timeout\","
          << "\"win32\":" << win32 << "}";
        return o.str();
    }

    const LONG code = InterlockedCompareExchange(&g_lastActionCode, 0, 0);
    std::ostringstream o;
    o << "{\"ok\":" << (code == 1 ? "true" : "false")
      << ",\"cmd\":\"ATTACK_ID\",\"result\":\"" << actionCodeText(code)
      << "\",\"requestedId\":" << creatureId
      << ",\"targetId\":" << g_lastActionTargetId
      << ",\"gameThreadResult\":" << (code == 1 ? 1 : 0) << "}";
    return o.str();
}
std::string attackSelectedResponse() {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"ATTACK_SELECTED","error":"build_not_validated"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"ATTACK_SELECTED","error":"dispatcher_not_ready"})";

    g_lastActionTargetId = 0;
    g_pendingAttackTarget = 0;
    g_pendingAttackRequestedId = 0;

    DWORD win32{};
    const bool dispatched = dispatchAttackToGameThread(win32);
    if (!dispatched) {
        InterlockedExchange(&g_lastActionCode, 8);
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"ATTACK_SELECTED\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    const LONG code = InterlockedCompareExchange(&g_lastActionCode, 0, 0);
    std::ostringstream o;
    o << "{\"ok\":" << (code == 1 ? "true" : "false")
      << ",\"cmd\":\"ATTACK_SELECTED\",\"result\":\"" << actionCodeText(code)
      << "\",\"targetId\":" << g_lastActionTargetId
      << ",\"gameThreadResult\":" << (code == 1 ? 1 : 0) << "}";
    return o.str();
}
std::string handleRequest(const std::string& request) {
    const auto cmd = commandFromRequest(request);
    if (cmd == "HELLO") {
        std::ostringstream o;
        o << "{\"ok\":true,\"cmd\":\"HELLO\",\"bridge\":\"CromeRamoniaBridge\","
          << "\"bridgeVersion\":13,\"protocol\":1,\"pid\":" << GetCurrentProcessId()
          << ",\"pipe\":\"" << jsonEscape(pipeName()) << "\","
          << "\"status\":\"" << jsonEscape(g_status) << "\","
          << "\"profileReady\":" << (g_profile.has_value() ? "true" : "false")
          << ",\"buildMatched\":" << (g_buildMatched ? "true" : "false")
          << ",\"dispatcherReady\":" << (dispatcherReady() ? "true" : "false")
          << ",\"dispatcherMode\":\"on_demand\""
          << ",\"dispatcherHookActive\":" << (g_gameThreadHook ? "true" : "false");
        if (!g_profileError.empty())
            o << ",\"profileError\":\"" << jsonEscape(g_profileError) << "\"";
        o << "}";
        return o.str();
    }

    if (!g_profile) {
        return "{\"ok\":false,\"cmd\":\"" + jsonEscape(cmd) +
               "\",\"error\":\"profile_not_ready\",\"detail\":\"" +
               jsonEscape(g_profileError) + "\"}";
    }

    const auto& p = *g_profile;
    if (cmd == "BUILD_INFO") {
        std::ostringstream o;
        o << "{\"ok\":true,\"cmd\":\"BUILD_INFO\",\"schema\":\"" << jsonEscape(p.schema)
          << "\",\"profileVersion\":" << p.profileVersion
          << ",\"architecture\":\"" << jsonEscape(p.architecture)
          << "\",\"timestamp\":\"" << jsonEscape(p.timestamp)
          << "\",\"sizeOfImage\":\"" << jsonEscape(p.sizeOfImage)
          << "\",\"textHash\":\"" << jsonEscape(p.textHash) << "\","
          << "\"runtimeTimestamp\":\"0x" << std::hex << std::uppercase << g_imageTimestamp
          << "\",\"runtimeSizeOfImage\":\"0x" << g_imageSize << std::dec
          << "\",\"matched\":" << (g_buildMatched ? "true" : "false") << "}";
        return o.str();
    }
    if (cmd == "GET_PLAYER") return playerResponse(p, g_base);
    if (cmd == "GET_COMBAT_TARGETS") return combatTargetsResponse(p);
    if (cmd == "GET_VISIBLE_MONSTERS") return visibleMonstersResponse();
    if (cmd == "GET_TILE_MONSTERS") return tileMonstersResponse();
    if (cmd == "LOOT_MARK") return lootMarkResponse();
    if (cmd == "LOOT_DIFF") return lootDiffResponse();
    if (cmd == "ATTACK_SELECTED") return attackSelectedResponse();
    if (cmd == "ATTACK_ID") {
        const auto id = uint32FieldFromRequest(request, "creatureId");
        if (!id)
            return R"({"ok":false,"cmd":"ATTACK_ID","error":"invalid_creature_id"})";
        return attackIdResponse(*id);
    }
    return "{\"ok\":false,\"error\":\"unknown_command\",\"cmd\":\"" + jsonEscape(cmd) + "\"}";
}

void serveOne() {
    const auto name = pipeName();
    HANDLE pipe = CreateNamedPipeA(
        name.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 8192, 8192, 1000, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        const auto err = GetLastError();
        g_status = "pipe_create_failed";
        logLine("[PIPE] CreateNamedPipe failed Win32=" + std::to_string(err) + " name=" + name);
        writeStatusFile();
        Sleep(500);
        return;
    }

    if (g_status != "listening") {
        g_status = "listening";
        logLine("[PIPE] listening name=" + name);
        writeStatusFile();
    }

    const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE :
        (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

    if (connected) {
        char buffer[4096]{};
        DWORD read{};
        if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr) && read) {
            std::string request(buffer, buffer + read);
            const auto response = handleRequest(request);
            DWORD written{};
            WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, nullptr);
            FlushFileBuffers(pipe);
        }
    }

    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

DWORD WINAPI bridgeThread(LPVOID) {
    try {
        // Reset diagnostics on every fresh DLL load.
        try {
            std::ofstream clear(bridgeLogPath(), std::ios::trunc | std::ios::binary);
            if (clear) clear << "CromeRamoniaBridge V13 startup\r\n";
        } catch (...) {}

        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        logLine("[START] pid=" + std::to_string(GetCurrentProcessId()));
        {
            std::ostringstream o;
            o << "[START] moduleBase=0x" << std::hex << std::uppercase << g_base;
            logLine(o.str());
        }

        try {
            const auto profilePath = findProfilePath();
            logLine("[PROFILE] path=" + profilePath);
            auto profile = loadProfile(profilePath);
            if (profile.architecture != "x86")
                throw std::runtime_error("profile architecture is not x86");
            logLine("[PROFILE] loaded schema=" + profile.schema +
                    " version=" + std::to_string(profile.profileVersion));
            g_profile = std::move(profile);
            g_profileError.clear();

            if (!readCurrentImageFingerprint())
                throw std::runtime_error("cannot read current image fingerprint");

            g_buildMatched =
                g_imageTimestamp == g_profile->expectedTimestamp &&
                g_imageSize == g_profile->expectedImageSize;

            {
                std::ostringstream fp;
                fp << "[BUILD] runtime timestamp=0x" << std::hex << std::uppercase
                   << g_imageTimestamp << " size=0x" << g_imageSize
                   << " matched=" << (g_buildMatched ? "true" : "false");
                logLine(fp.str());
            }

            if (!g_buildMatched)
                throw std::runtime_error("runtime build does not match V37 profile");

            if (!prepareGameThreadDispatcher())
                throw std::runtime_error("game-thread target prepare failed");

            g_status = "runtime_ready";
        } catch (const std::exception& e) {
            g_buildMatched = false;
            g_profileError = e.what();
            g_status = "profile_error";
            logLine("[PROFILE ERROR] " + g_profileError);
        } catch (...) {
            g_buildMatched = false;
            g_profileError = "unknown profile initialization error";
            g_status = "profile_error";
            logLine("[PROFILE ERROR] unknown exception");
        }

        writeStatusFile();

        // Critical V2 change: always expose the pipe, even if profile loading failed.
        while (InterlockedCompareExchange(&g_running, 1, 1))
            serveOne();

    } catch (const std::exception& e) {
        g_status = "bridge_thread_error";
        g_profileError = e.what();
        logLine("[FATAL] " + g_profileError);
        writeStatusFile();
    } catch (...) {
        g_status = "bridge_thread_error";
        g_profileError = "unknown bridge thread error";
        logLine("[FATAL] unknown exception");
        writeStatusFile();
    }

    cleanupOnDemandDispatcher();
    g_gameThreadId = 0;
    g_gameWindow = nullptr;
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        InterlockedExchange(&g_running, 1);
        if (HANDLE h = CreateThread(nullptr, 0, bridgeThread, nullptr, 0, nullptr)) {
            CloseHandle(h);
        } else {
            g_status = "thread_create_failed";
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_running, 0);
    }
    return TRUE;
}
