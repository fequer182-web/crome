#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MiniJson.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <set>
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
constexpr UINT kActionMessage = WM_APP + 0x620;

enum PendingActionKind : LONG {
    PendingNone = 0,
    PendingAttack = 1,
    PendingUse = 2,
    PendingMove = 3,
    PendingTalk = 4,
    PendingUseWithSelf = 5,
    PendingAutoWalk = 6
};

volatile LONG g_lastActionCode{};
volatile LONG g_actionTokenCounter{};
volatile LONG g_pendingActionToken{};
volatile LONG g_pendingActionKind{};
std::uint32_t g_lastActionTargetId{};
std::uintptr_t g_pendingAttackTarget{};
std::uint32_t g_pendingAttackRequestedId{};

std::uintptr_t g_pendingUseTile{};
std::uintptr_t g_pendingUseContainer{};
std::uint32_t g_pendingUseContainerId{};
std::uintptr_t g_pendingUseHolder{};
std::uintptr_t g_pendingUseItem{};
std::uint16_t g_pendingUseItemId{};

std::uintptr_t g_pendingMoveSourceContainer{};
std::uintptr_t g_pendingMoveHolder{};
std::uintptr_t g_pendingMoveItem{};
std::uint16_t g_pendingMoveItemId{};
std::uint32_t g_pendingMoveCount{};
std::uintptr_t g_pendingMoveTargetContainer{};
std::uint32_t g_pendingMoveTargetContainerId{};
std::uint8_t g_pendingMoveTargetSlot{};

std::uint32_t g_pendingTalkMode{1};
std::uint32_t g_pendingTalkChannel{};
char g_pendingTalkMessage[16]{};

std::uint32_t g_pendingWalkX{};
std::uint32_t g_pendingWalkY{};
std::uint8_t g_pendingWalkZ{};

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
    std::uintptr_t localPlayerVtableRva{};
    std::uintptr_t itemVtableRva{};
    std::uintptr_t containerVtableRva{};
    std::size_t itemIdOffset{};
    std::size_t itemCountOffset{};
    bool itemContractReady{};

    std::size_t openContainersOffset{};
    std::size_t containerIdOffset{};
    std::size_t containerCapacityOffset{};
    std::size_t containerNameOffset{};
    std::size_t containerItemsOffset{};
    std::size_t containerItemStride{};
    bool containerContractReady{};

    std::uintptr_t useMethodRva{};
    bool useRuntimeConfirmed{};
    bool useItemSharedRefConfirmed{};

    std::uintptr_t lookMethodRva{};
    bool lookRuntimeConfirmed{};
    bool lookItemSharedRefConfirmed{};

    std::uintptr_t useWithMethodRva{};
    bool useWithRuntimeConfirmed{};
    bool useWithItemSharedRefConfirmed{};
    bool useWithThingSharedRefConfirmed{};

    std::uintptr_t moveMethodRva{};
    bool moveRuntimeConfirmed{};
    bool moveItemSharedRefConfirmed{};
    bool movePositionRefConfirmed{};
    bool moveCountConfirmed{};

    std::uintptr_t autoWalkMethodRva{};
    bool autoWalkRuntimeConfirmed{};
    bool autoWalkDirectionsVectorRefConfirmed{};

    std::uintptr_t tileVtableRva{};
    std::size_t tileThingsOffset{};
    std::size_t tileThingsEndOffset{};
    std::size_t tileThingsCapOffset{};
    std::size_t tileThingsElemSize{};
    bool tileThingsRuntimeConfirmed{};

    std::uintptr_t uiMapVtableRva{};
    std::uintptr_t mapViewVtableRva{};
    std::size_t uiMapToMapViewOffset{};
    std::size_t mapViewDrawLightsOffset{};
    std::size_t mapViewMinimumAmbientOffset{};
    bool worldLightContractReady{};

    std::uintptr_t talkChannelMethodRva{};
    bool talkChannelRuntimeConfirmed{};
    bool talkModeArgConfirmed{};
    bool talkChannelArgConfirmed{};
    bool talkMessageMsvcStringRefConfirmed{};
    std::uint32_t talkSayModeValue{1};
    std::uint32_t talkDefaultChannel{};

    std::uintptr_t attackMethodRva{};
    bool attackRuntimeConfirmed{};
    bool attackCreatureSharedValueConfirmed{};
    std::size_t sharedObjectRefCountOffset{};
    bool attackCancelFalseConfirmed{};

    struct Field {
        std::size_t offset{};
        std::string type;
        bool valid{};
    };
    Field health, maxHealth, mana, maxMana, soul;
};

std::optional<Profile> g_profile;

struct WorldLightTargetState {
    std::uintptr_t uiMap{};
    std::uintptr_t mapView{};
    float originalAmbient{};
    bool originalSaved{};
};
std::vector<WorldLightTargetState> g_worldLightTargets;
bool g_worldLightEnabled{};
constexpr float kWorldLightHackAmbient = 1.0f;

// One-shot real Game::look capture. No breakpoint/VEH remains armed while idle.
SRWLOCK g_lookCaptureLock = SRWLOCK_INIT;
PVOID g_lookVehHandle{};
HANDLE g_lookCaptureEvent{};
volatile LONG g_lookCaptureWorkerActive{};
volatile LONG g_lookCaptureArmed{};
volatile LONG g_lookCaptureCaptured{};
std::uintptr_t g_lookCaptureAddress{};
std::uint8_t g_lookCaptureOriginalByte{};
std::uint32_t g_lookCapturedId{};
std::uintptr_t g_lookCapturedHolder{};
std::uintptr_t g_lookCapturedItem{};
ULONGLONG g_lookCapturedTick{};

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

    const auto* localPlayer = child(rtti, "LocalPlayer");
    const auto* localPlayerVtables = child(localPlayer, "vtables");
    if (localPlayerVtables && localPlayerVtables->isArray() &&
        !localPlayerVtables->array().empty()) {
        const auto n=integerValue(&localPlayerVtables->array().front());
        if (n) p.localPlayerVtableRva=static_cast<std::uintptr_t>(*n);
    }

    const auto* item = child(rtti, "Item");
    const auto* itemVtables = child(item, "vtables");
    if (itemVtables && itemVtables->isArray() && !itemVtables->array().empty()) {
        const auto n = integerValue(&itemVtables->array().front());
        if (n) p.itemVtableRva = static_cast<std::uintptr_t>(*n);
    }

    const auto* container = child(rtti, "Container");
    const auto* containerVtables = child(container, "vtables");
    if (containerVtables && containerVtables->isArray() && !containerVtables->array().empty()) {
        const auto n = integerValue(&containerVtables->array().front());
        if (n) p.containerVtableRva = static_cast<std::uintptr_t>(*n);
    }

    const auto* uiMap = child(rtti, "UIMap");
    const auto* uiMapVtables = child(uiMap, "vtables");
    if (uiMapVtables && uiMapVtables->isArray() && !uiMapVtables->array().empty()) {
        const auto n = integerValue(&uiMapVtables->array().front());
        if (n) p.uiMapVtableRva = static_cast<std::uintptr_t>(*n);
    }

    const auto* mapView = child(rtti, "MapView");
    const auto* mapViewVtables = child(mapView, "vtables");
    if (mapViewVtables && mapViewVtables->isArray() && !mapViewVtables->array().empty()) {
        const auto n = integerValue(&mapViewVtables->array().front());
        if (n) p.mapViewVtableRva = static_cast<std::uintptr_t>(*n);
    }

    const auto* actions = root.get("actions");
    const auto* attack = child(actions, "attack");
    if (attack && attack->isObject()) {
        if (const auto n = integerValue(attack->get("methodRva")))
            p.attackMethodRva = static_cast<std::uintptr_t>(*n);
        p.attackRuntimeConfirmed = stringValue(attack->get("status")) == "runtime_confirmed";

        const auto* args = attack->get("args");
        if (args && args->isArray() && args->array().size() >= 2) {
            const auto& creature = args->array()[0];
            const auto& cancel = args->array()[1];

            p.attackCreatureSharedValueConfirmed =
                creature.isObject() &&
                stringValue(creature.get("semantic")) == "Creature" &&
                stringValue(creature.get("abiType")) == "shared_object_ptr_value" &&
                stringValue(creature.get("status")) == "runtime_confirmed";

            if (const auto n = integerValue(
                    creature.isObject() ? creature.get("refCountOffset") : nullptr))
                p.sharedObjectRefCountOffset = static_cast<std::size_t>(*n);

            const auto* required = cancel.isObject() ? cancel.get("observedRequiredValue") : nullptr;
            p.attackCancelFalseConfirmed =
                cancel.isObject() &&
                stringValue(cancel.get("semantic")) == "Cancel" &&
                stringValue(cancel.get("abiType")) == "bool" &&
                stringValue(cancel.get("status")) == "runtime_confirmed" &&
                required && required->isBool() && required->boolean() == false;
        }
    }


    const auto* use = child(actions, "use");
    if (use && use->isObject()) {
        if (const auto n = integerValue(use->get("methodRva")))
            p.useMethodRva = static_cast<std::uintptr_t>(*n);
        p.useRuntimeConfirmed = stringValue(use->get("status")) == "runtime_confirmed";
        const auto* args = use->get("args");
        if (args && args->isArray() && !args->array().empty()) {
            const auto& a0 = args->array()[0];
            p.useItemSharedRefConfirmed =
                a0.isObject() &&
                stringValue(a0.get("semantic")) == "Item" &&
                stringValue(a0.get("abiType")) == "shared_object_ptr_ref" &&
                stringValue(a0.get("status")) == "runtime_confirmed";
        }
    }

    const auto* look = child(actions, "look");
    if (look && look->isObject()) {
        if (const auto n=integerValue(look->get("methodRva")))
            p.lookMethodRva=static_cast<std::uintptr_t>(*n);
        p.lookRuntimeConfirmed=
            stringValue(look->get("status"))=="runtime_confirmed";

        const auto* args=look->get("args");
        if (args && args->isArray() && !args->array().empty()) {
            const auto& a0=args->array()[0];
            p.lookItemSharedRefConfirmed=
                a0.isObject() &&
                stringValue(a0.get("semantic"))=="Item" &&
                stringValue(a0.get("abiType"))=="shared_object_ptr_ref" &&
                stringValue(a0.get("status"))=="runtime_confirmed";
        }
    }

    const auto* useWith = child(actions, "useWith");
    if (useWith && useWith->isObject()) {
        if (const auto n=integerValue(useWith->get("methodRva")))
            p.useWithMethodRva=static_cast<std::uintptr_t>(*n);
        p.useWithRuntimeConfirmed=
            stringValue(useWith->get("status"))=="runtime_confirmed";

        const auto* args=useWith->get("args");
        if (args && args->isArray() && args->array().size()>=2) {
            const auto& a0=args->array()[0];
            const auto& a1=args->array()[1];
            p.useWithItemSharedRefConfirmed=
                a0.isObject() &&
                stringValue(a0.get("semantic"))=="Item" &&
                stringValue(a0.get("abiType"))=="shared_object_ptr_ref" &&
                stringValue(a0.get("status"))=="runtime_confirmed";
            p.useWithThingSharedRefConfirmed=
                a1.isObject() &&
                stringValue(a1.get("semantic"))=="Thing" &&
                stringValue(a1.get("abiType"))=="shared_object_ptr_ref" &&
                stringValue(a1.get("status"))=="runtime_confirmed";
        }
    }

    const auto* move = child(actions, "move");
    if (move && move->isObject()) {
        if (const auto n = integerValue(move->get("methodRva")))
            p.moveMethodRva = static_cast<std::uintptr_t>(*n);
        p.moveRuntimeConfirmed = stringValue(move->get("status")) == "runtime_confirmed";
        const auto* args = move->get("args");
        if (args && args->isArray() && args->array().size() >= 3) {
            const auto& a0 = args->array()[0];
            const auto& a1 = args->array()[1];
            const auto& a2 = args->array()[2];
            p.moveItemSharedRefConfirmed =
                a0.isObject() &&
                stringValue(a0.get("semantic")) == "Item" &&
                stringValue(a0.get("abiType")) == "shared_object_ptr_ref" &&
                stringValue(a0.get("status")) == "runtime_confirmed";
            p.movePositionRefConfirmed =
                a1.isObject() &&
                stringValue(a1.get("semantic")) == "Position" &&
                stringValue(a1.get("abiType")) == "position_u32_u32_u8_ref" &&
                stringValue(a1.get("status")) == "runtime_confirmed";
            p.moveCountConfirmed =
                a2.isObject() &&
                stringValue(a2.get("semantic")) == "Count" &&
                stringValue(a2.get("abiType")) == "u32" &&
                stringValue(a2.get("status")) == "runtime_confirmed";
        }
    }

    const auto* autoWalk = child(actions, "autoWalk");
    if (autoWalk && autoWalk->isObject()) {
        if (const auto n = integerValue(autoWalk->get("methodRva")))
            p.autoWalkMethodRva = static_cast<std::uintptr_t>(*n);
        p.autoWalkRuntimeConfirmed =
            stringValue(autoWalk->get("status")) == "runtime_confirmed";

        const auto* args = autoWalk->get("args");
        if (args && args->isArray() && !args->array().empty()) {
            const auto& a0 = args->array()[0];
            p.autoWalkDirectionsVectorRefConfirmed =
                a0.isObject() &&
                stringValue(a0.get("semantic")) == "Directions" &&
                stringValue(a0.get("abiType")) == "vector_direction_u32_ref" &&
                stringValue(a0.get("status")) == "runtime_confirmed";
        }
    }

    const auto* talkChannel = child(actions, "talkChannel");
    if (talkChannel && talkChannel->isObject()) {
        if (const auto n = integerValue(talkChannel->get("methodRva")))
            p.talkChannelMethodRva = static_cast<std::uintptr_t>(*n);
        p.talkChannelRuntimeConfirmed =
            stringValue(talkChannel->get("status")) == "runtime_confirmed";
        if (const auto n=integerValue(talkChannel->get("messageSayValue")))
            p.talkSayModeValue=static_cast<std::uint32_t>(*n);
        if (const auto n=integerValue(talkChannel->get("defaultChannel")))
            p.talkDefaultChannel=static_cast<std::uint32_t>(*n);

        const auto* args = talkChannel->get("args");
        if (args && args->isArray() && args->array().size() >= 3) {
            const auto& a0=args->array()[0];
            const auto& a1=args->array()[1];
            const auto& a2=args->array()[2];
            p.talkModeArgConfirmed =
                a0.isObject() &&
                stringValue(a0.get("semantic"))=="MessageMode" &&
                stringValue(a0.get("abiType"))=="u32" &&
                stringValue(a0.get("status"))=="runtime_confirmed";
            p.talkChannelArgConfirmed =
                a1.isObject() &&
                stringValue(a1.get("semantic"))=="Channel" &&
                stringValue(a1.get("abiType"))=="u32" &&
                stringValue(a1.get("status"))=="runtime_confirmed";
            p.talkMessageMsvcStringRefConfirmed =
                a2.isObject() &&
                stringValue(a2.get("semantic"))=="Message" &&
                stringValue(a2.get("abiType"))=="msvc_string_ref" &&
                stringValue(a2.get("status"))=="runtime_confirmed";
        }
    }

    const auto* runtime = root.get("runtime");

    const auto* worldLight = child(runtime, "worldLight");
    if (worldLight && worldLight->isObject()) {
        if (const auto n = integerValue(worldLight->get("uiMapToMapView")))
            p.uiMapToMapViewOffset = static_cast<std::size_t>(*n);

        const auto* drawLights = child(worldLight, "drawLights");
        if (const auto n = integerValue(child(drawLights, "offset")))
            p.mapViewDrawLightsOffset = static_cast<std::size_t>(*n);

        const auto* minimumAmbient = child(worldLight, "minimumAmbient");
        if (const auto n = integerValue(child(minimumAmbient, "offset")))
            p.mapViewMinimumAmbientOffset = static_cast<std::size_t>(*n);

        const auto contract = stringValue(worldLight->get("staticContractStatus"));
        p.worldLightContractReady =
            p.uiMapVtableRva &&
            p.mapViewVtableRva &&
            p.uiMapToMapViewOffset &&
            p.mapViewMinimumAmbientOffset &&
            stringValue(child(minimumAmbient, "type")) == "float" &&
            stringValue(child(drawLights, "type")) == "u8" &&
            contract == "semantic_uimap_mapview_setter_getter_contract";
    }

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
    p.soul = readField(fields, "soul");

    const auto itemId = readField(fields, "itemId");
    const auto itemCount = readField(fields, "itemCount");
    if (itemId.valid && itemCount.valid &&
        itemId.type == "u16" && itemCount.type == "u16" &&
        p.itemVtableRva) {
        p.itemIdOffset = itemId.offset;
        p.itemCountOffset = itemCount.offset;
        p.itemContractReady = true;
    }

    const auto openContainers = readField(fields, "openContainersMap");
    const auto containerId = readField(fields, "containerId");
    const auto containerCapacity = readField(fields, "containerCapacity");
    const auto containerName = readField(fields, "containerName");
    const auto containerItems = readField(fields, "containerItems");
    const auto containerItemStride = readField(fields, "containerItemStride");
    if (openContainers.valid && containerId.valid && containerCapacity.valid &&
        containerName.valid && containerItems.valid && containerItemStride.valid &&
        openContainers.type == "inline_registry" &&
        containerId.type == "u32" &&
        containerCapacity.type == "u32" &&
        containerName.type == "msvc_string" &&
        containerItems.type == "deque" &&
        containerItemStride.type == "bytes" &&
        containerItemStride.offset == 4 &&
        p.containerVtableRva) {
        p.openContainersOffset = openContainers.offset;
        p.containerIdOffset = containerId.offset;
        p.containerCapacityOffset = containerCapacity.offset;
        p.containerNameOffset = containerName.offset;
        p.containerItemsOffset = containerItems.offset;
        p.containerItemStride = containerItemStride.offset;
        p.containerContractReady = true;
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
        !p.attackRuntimeConfirmed || !p.attackCreatureSharedValueConfirmed ||
        !p.sharedObjectRefCountOffset || !p.attackCancelFalseConfirmed)
        throw std::runtime_error("V39 target/attack ownership contract incomplete");

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

bool isWritableAddress(std::uintptr_t address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS))
        return false;
    const DWORD p = mbi.Protect & 0xFFu;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

template<class T>
bool safeWrite(std::uintptr_t address, const T& value) {
    if (!isWritableAddress(address)) return false;
    SIZE_T written{};
    return WriteProcessMemory(
               GetCurrentProcess(),
               reinterpret_cast<LPVOID>(address),
               &value,
               sizeof(value),
               &written) &&
           written == sizeof(value);
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

struct MovePosition {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint8_t z{};
    std::uint8_t pad[3]{};
};
static_assert(sizeof(MovePosition) == 12, "MovePosition ABI must be 12 bytes");

#if defined(__GNUC__)
typedef void (__attribute__((thiscall)) *GameAttackFn)(void*, void*, bool);
typedef void (__attribute__((thiscall)) *GameUseFn)(void*, void*);
typedef void (__attribute__((thiscall)) *GameUseWithFn)(void*, void*, void*);
typedef void (__attribute__((thiscall)) *GameMoveFn)(void*, void*, const MovePosition*, std::uint32_t);
typedef void (__attribute__((thiscall)) *GameTalkChannelFn)(void*, std::uint32_t, std::uint32_t, const void*);
#else
typedef void (__thiscall *GameAttackFn)(void*, void*, bool);
typedef void (__thiscall *GameUseFn)(void*, void*);
typedef void (__thiscall *GameUseWithFn)(void*, void*, void*);
typedef void (__thiscall *GameMoveFn)(void*, void*, const MovePosition*, std::uint32_t);
typedef void (__thiscall *GameTalkChannelFn)(void*, std::uint32_t, std::uint32_t, const void*);
#endif

struct MsvcSmallString32 {
    char storage[16]{};
    std::uint32_t size{};
    std::uint32_t capacity{15};
};
static_assert(sizeof(MsvcSmallString32)==24,"MSVC x86 string SSO ABI must be 24 bytes");

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

void invalidateTileCacheForFloorChange(
    std::uint32_t px,
    std::uint32_t py,
    std::uint8_t pz) {

    AcquireSRWLockExclusive(&g_tileCacheLock);
    g_tileCache.clear();
    g_tileCacheTick = 0;
    g_tileCacheOriginX = px;
    g_tileCacheOriginY = py;
    g_tileCacheOriginZ = pz;
    ++g_tileCacheGeneration;
    ReleaseSRWLockExclusive(&g_tileCacheLock);

    logLine("[TILE CACHE] invalidated immediately on floor change -> z=" +
            std::to_string(static_cast<unsigned>(pz)));
}

bool ensureTileCache(std::uint32_t px, std::uint32_t py, std::uint8_t pz,
                     bool force = false) {
    const auto info = tileCacheInfo();
    const auto dx = px > info.originX ? px - info.originX : info.originX - px;
    const auto dy = py > info.originY ? py - info.originY : info.originY - py;
    const auto moved = std::max(dx, dy);

    // Floor changes are special: never reuse Tile* addresses from the
    // previous z, even briefly. Invalidate immediately and let the next poll
    // rebuild the cache after the client has had time to settle.
    if (info.count != 0 && info.originZ != pz) {
        invalidateTileCacheForFloorChange(px, py, pz);
        return false;
    }

    bool refresh = force || info.count == 0;
    if (!refresh && info.ageMs >= kTileCacheMaxAgeMs) refresh = true;
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

struct LootTargetTileFrame {
    std::uintptr_t tileAddress{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint8_t z{};
    ULONGLONG tick{};
    std::vector<TileItemDiagnostic> items;
};

struct LootBaselineState {
    bool valid{};
    std::uint64_t tileGeneration{};
    std::uint32_t trackedTargetId{};
    std::uintptr_t trackedTargetTile{};
    std::vector<LootTargetTileFrame> targetTileHistory;
    std::vector<std::uintptr_t> tiles;
    std::vector<TileItemDiagnostic> items;
    std::vector<TileMonsterDiagnostic> monsters;
};

SRWLOCK g_lootBaselineLock = SRWLOCK_INIT;
LootBaselineState g_lootBaseline;

SRWLOCK g_lootCandidateLock = SRWLOCK_INIT;
std::vector<TileItemDiagnostic> g_lastLootCandidates;

bool safeCopyBytes(std::uintptr_t address, void* out, std::size_t size) {
    if (!address || !out || !size) return false;
    SIZE_T got{};
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<LPCVOID>(address),
                             out, size, &got) && got == size;
}

bool looksHeapPointer(std::uintptr_t address) {
    if (address < 0x10000u || address >= 0x7FFF0000u) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)))
        return false;
    return mbi.State == MEM_COMMIT &&
           (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED) &&
           readableProtection(mbi.Protect);
}

bool exactObjectVtable(std::uintptr_t object, std::uintptr_t vtableRva) {
    if (!object || !vtableRva) return false;
    std::uintptr_t vt{};
    return safeRead(object, vt) && vt == g_base + vtableRva;
}

bool validateWorldLightTarget(std::uintptr_t uiMap,
                              std::uintptr_t& mapViewOut,
                              float* ambientOut = nullptr) {
    mapViewOut = 0;
    if (!g_profile || !g_profile->worldLightContractReady ||
        !uiMap || !exactObjectVtable(uiMap,g_profile->uiMapVtableRva))
        return false;

    std::uintptr_t mapView{};
    if (!safeRead(uiMap + g_profile->uiMapToMapViewOffset,mapView) ||
        !mapView ||
        !exactObjectVtable(mapView,g_profile->mapViewVtableRva))
        return false;

    float ambient{};
    if (!safeRead(mapView + g_profile->mapViewMinimumAmbientOffset,ambient) ||
        !std::isfinite(ambient) || ambient < -0.01f || ambient > 2.0f)
        return false;

    if (g_profile->mapViewDrawLightsOffset) {
        std::uint8_t draw{};
        if (!safeRead(mapView + g_profile->mapViewDrawLightsOffset,draw) || draw > 1u)
            return false;
    }

    mapViewOut=mapView;
    if (ambientOut) *ambientOut=ambient;
    return true;
}

std::vector<WorldLightTargetState> discoverWorldLightTargets() {
    std::vector<WorldLightTargetState> found;
    if (!g_profile || !g_profile->worldLightContractReady)
        return found;

    const std::uint32_t wantedUiMapVtable =
        static_cast<std::uint32_t>(g_base + g_profile->uiMapVtableRva);

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto address = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const auto maxAddress =
        reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    constexpr SIZE_T kChunk = 1u << 20;
    std::vector<std::uint8_t> buffer(kChunk);

    while (address < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(address),&mbi,sizeof(mbi)))
            break;

        const auto regionBase=
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEnd=regionBase+mbi.RegionSize;
        if (regionEnd<=address) break;

        if (mbi.State==MEM_COMMIT &&
            mbi.Type==MEM_PRIVATE &&
            readableProtection(mbi.Protect)) {

            for (auto chunkBase=regionBase; chunkBase<regionEnd;) {
                const SIZE_T want=static_cast<SIZE_T>(
                    std::min<std::uintptr_t>(kChunk,regionEnd-chunkBase));
                SIZE_T got{};
                if (ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<LPCVOID>(chunkBase),
                        buffer.data(),want,&got) &&
                    got>=sizeof(std::uint32_t)) {

                    for (SIZE_T i=0;i+sizeof(std::uint32_t)<=got;i+=4) {
                        std::uint32_t vt{};
                        std::memcpy(&vt,buffer.data()+i,sizeof(vt));
                        if (vt!=wantedUiMapVtable) continue;

                        const auto uiMap=chunkBase+i;
                        std::uintptr_t mapView{};
                        float ambient{};
                        if (!validateWorldLightTarget(uiMap,mapView,&ambient))
                            continue;

                        bool duplicate=false;
                        for (const auto& existing:found)
                            if (existing.mapView==mapView) {
                                duplicate=true;
                                break;
                            }
                        if (duplicate) continue;

                        WorldLightTargetState state;
                        state.uiMap=uiMap;
                        state.mapView=mapView;
                        state.originalAmbient=ambient;
                        state.originalSaved=false;
                        found.push_back(state);
                    }
                }
                chunkBase += want ? want : 1;
            }
        }
        address=regionEnd;
    }

    return found;
}

bool ensureWorldLightTargets() {
    if (!g_profile || !g_profile->worldLightContractReady)
        return false;

    bool valid=!g_worldLightTargets.empty();
    if (valid) {
        for (const auto& target:g_worldLightTargets) {
            std::uintptr_t currentMapView{};
            if (!validateWorldLightTarget(target.uiMap,currentMapView,nullptr) ||
                currentMapView!=target.mapView) {
                valid=false;
                break;
            }
        }
    }

    if (!valid)
        g_worldLightTargets=discoverWorldLightTargets();

    return !g_worldLightTargets.empty();
}

bool applyWorldLightHack() {
    if (!ensureWorldLightTargets())
        return false;

    bool wroteAny=false;
    for (auto& target:g_worldLightTargets) {
        std::uintptr_t currentMapView{};
        float currentAmbient{};
        if (!validateWorldLightTarget(
                target.uiMap,currentMapView,&currentAmbient) ||
            currentMapView!=target.mapView)
            continue;

        if (!target.originalSaved) {
            target.originalAmbient=currentAmbient;
            target.originalSaved=true;
        }

        if (safeWrite(
                target.mapView + g_profile->mapViewMinimumAmbientOffset,
                kWorldLightHackAmbient))
            wroteAny=true;
    }

    g_worldLightEnabled=wroteAny;
    return wroteAny;
}

bool restoreWorldLightHack() {
    bool restoredAny=false;
    for (auto& target:g_worldLightTargets) {
        if (!target.originalSaved) continue;

        std::uintptr_t currentMapView{};
        if (!validateWorldLightTarget(
                target.uiMap,currentMapView,nullptr) ||
            currentMapView!=target.mapView)
            continue;

        if (safeWrite(
                target.mapView + g_profile->mapViewMinimumAmbientOffset,
                target.originalAmbient))
            restoredAny=true;

        target.originalSaved=false;
    }

    g_worldLightEnabled=false;
    return restoredAny || g_worldLightTargets.empty();
}

std::string worldLightStatusResponse(const char* cmd) {
    std::ostringstream o;
    const bool resolved=ensureWorldLightTargets();
    o << "{\"ok\":" << (resolved ? "true" : "false")
      << ",\"cmd\":\"" << cmd << "\""
      << ",\"contractReady\":"
      << ((g_profile && g_profile->worldLightContractReady) ? "true" : "false")
      << ",\"enabled\":" << (g_worldLightEnabled ? "true" : "false")
      << ",\"targets\":" << g_worldLightTargets.size()
      << ",\"forcedAmbient\":" << kWorldLightHackAmbient;

    if (resolved && !g_worldLightTargets.empty()) {
        float ambient{};
        safeRead(
            g_worldLightTargets.front().mapView +
                g_profile->mapViewMinimumAmbientOffset,
            ambient);
        o << ",\"ambient\":" << ambient;
    }

    if (!resolved)
        o << ",\"error\":\"world_light_target_not_found\"";

    o << "}";
    return o.str();
}

std::string setWorldLightResponse(bool enabled) {
    if (!g_profile || !g_profile->worldLightContractReady)
        return R"({"ok":false,"cmd":"SET_LIGHT","error":"world_light_contract_not_ready"})";

    const bool ok=enabled ? applyWorldLightHack() : restoreWorldLightHack();
    logLine(std::string("[LIGHT] ") +
            (enabled ? "enable" : "restore") +
            " ok=" + (ok ? "true" : "false") +
            " targets=" + std::to_string(g_worldLightTargets.size()));
    return worldLightStatusResponse("SET_LIGHT");
}

bool writeCodeByte(std::uintptr_t address,std::uint8_t value) {
    if (!address) return false;
    DWORD oldProtect{};
    if (!VirtualProtect(
            reinterpret_cast<void*>(address),
            1,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
        return false;

    *reinterpret_cast<volatile std::uint8_t*>(address)=value;
    FlushInstructionCache(
        GetCurrentProcess(),
        reinterpret_cast<void*>(address),
        1);

    DWORD ignored{};
    VirtualProtect(
        reinterpret_cast<void*>(address),
        1,
        oldProtect,
        &ignored);
    return true;
}

void restoreLookCaptureBreakpointLocked() {
    if (!g_lookCaptureArmed || !g_lookCaptureAddress)
        return;
    writeCodeByte(g_lookCaptureAddress,g_lookCaptureOriginalByte);
    InterlockedExchange(&g_lookCaptureArmed,0);
}

LONG CALLBACK lookCaptureVeh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;

    AcquireSRWLockExclusive(&g_lookCaptureLock);

    if (!g_lookCaptureArmed ||
        ep->ExceptionRecord->ExceptionAddress !=
            reinterpret_cast<void*>(g_lookCaptureAddress)) {
        ReleaseSRWLockExclusive(&g_lookCaptureLock);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // One-shot: restore original byte immediately before resuming Game::look.
    restoreLookCaptureBreakpointLocked();

    std::uintptr_t holderRef{};
    std::uintptr_t item{};
    std::uint16_t itemId{};
    std::uintptr_t itemVtable{};

    const auto esp=static_cast<std::uintptr_t>(ep->ContextRecord->Esp);
    const bool ok=
        g_profile &&
        g_profile->lookRuntimeConfirmed &&
        g_profile->lookItemSharedRefConfirmed &&
        safeRead(esp+sizeof(std::uintptr_t),holderRef) &&
        holderRef &&
        safeRead(holderRef,item) &&
        item &&
        safeRead(item,itemVtable) &&
        itemVtable>=g_base &&
        itemVtable<g_base+g_imageSize &&
        safeRead(item+g_profile->itemIdOffset,itemId) &&
        itemId!=0;

    if (ok) {
        g_lookCapturedHolder=holderRef;
        g_lookCapturedItem=item;
        g_lookCapturedId=itemId;
        g_lookCapturedTick=GetTickCount64();
        InterlockedExchange(&g_lookCaptureCaptured,1);
        logLine("[LOOK] captured itemId="+
                std::to_string(static_cast<unsigned>(itemId)));
    } else {
        g_lookCapturedHolder=0;
        g_lookCapturedItem=0;
        g_lookCapturedId=0;
        g_lookCapturedTick=GetTickCount64();
        InterlockedExchange(&g_lookCaptureCaptured,0);
        logLine("[LOOK] Game::look fired but ItemPtr validation failed");
    }

    // INT3 advances EIP by one byte. Rewind so the restored original first
    // instruction executes normally.
    ep->ContextRecord->Eip=
        static_cast<DWORD>(g_lookCaptureAddress);

    if (g_lookCaptureEvent)
        SetEvent(g_lookCaptureEvent);

    ReleaseSRWLockExclusive(&g_lookCaptureLock);
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI lookCaptureTimeoutThread(void*) {
    HANDLE event{};
    AcquireSRWLockShared(&g_lookCaptureLock);
    event=g_lookCaptureEvent;
    ReleaseSRWLockShared(&g_lookCaptureLock);

    if (event)
        WaitForSingleObject(event,8000);

    // Let the VEH finish returning before unregistering it.
    Sleep(40);

    AcquireSRWLockExclusive(&g_lookCaptureLock);
    restoreLookCaptureBreakpointLocked();

    if (g_lookVehHandle) {
        RemoveVectoredExceptionHandler(g_lookVehHandle);
        g_lookVehHandle=nullptr;
    }

    if (g_lookCaptureEvent) {
        CloseHandle(g_lookCaptureEvent);
        g_lookCaptureEvent=nullptr;
    }

    InterlockedExchange(&g_lookCaptureWorkerActive,0);
    ReleaseSRWLockExclusive(&g_lookCaptureLock);
    return 0;
}

void cleanupLookCapture(bool clearResult=false) {
    // Avoid closing the wait handle underneath an active timeout worker.
    AcquireSRWLockExclusive(&g_lookCaptureLock);
    restoreLookCaptureBreakpointLocked();

    if (!g_lookCaptureWorkerActive) {
        if (g_lookVehHandle) {
            RemoveVectoredExceptionHandler(g_lookVehHandle);
            g_lookVehHandle=nullptr;
        }
        if (g_lookCaptureEvent) {
            CloseHandle(g_lookCaptureEvent);
            g_lookCaptureEvent=nullptr;
        }
    }

    if (clearResult) {
        InterlockedExchange(&g_lookCaptureCaptured,0);
        g_lookCapturedId=0;
        g_lookCapturedHolder=0;
        g_lookCapturedItem=0;
        g_lookCapturedTick=0;
    }
    ReleaseSRWLockExclusive(&g_lookCaptureLock);
}

std::string armLookCaptureResponse() {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"build_not_validated"})";
    if (!g_profile->lookRuntimeConfirmed ||
        !g_profile->lookItemSharedRefConfirmed ||
        !g_profile->lookMethodRva ||
        !g_profile->itemContractReady)
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"look_contract_not_ready"})";

    if (InterlockedCompareExchange(
            &g_lookCaptureWorkerActive,1,0)!=0)
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"capture_busy"})";

    const auto lookAddress=g_base+g_profile->lookMethodRva;
    if (!isExecutableAddress(lookAddress)) {
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"look_function_not_executable"})";
    }

    std::uint8_t original{};
    if (!safeRead(lookAddress,original) || original==0xCC) {
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"look_first_byte_unavailable"})";
    }

    AcquireSRWLockExclusive(&g_lookCaptureLock);

    g_lookCaptureAddress=lookAddress;
    g_lookCaptureOriginalByte=original;
    g_lookCapturedId=0;
    g_lookCapturedHolder=0;
    g_lookCapturedItem=0;
    g_lookCapturedTick=0;
    InterlockedExchange(&g_lookCaptureCaptured,0);

    g_lookCaptureEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!g_lookCaptureEvent) {
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        ReleaseSRWLockExclusive(&g_lookCaptureLock);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"event_create_failed"})";
    }

    g_lookVehHandle=AddVectoredExceptionHandler(1,&lookCaptureVeh);
    if (!g_lookVehHandle) {
        CloseHandle(g_lookCaptureEvent);
        g_lookCaptureEvent=nullptr;
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        ReleaseSRWLockExclusive(&g_lookCaptureLock);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"veh_install_failed"})";
    }

    if (!writeCodeByte(lookAddress,0xCC)) {
        RemoveVectoredExceptionHandler(g_lookVehHandle);
        g_lookVehHandle=nullptr;
        CloseHandle(g_lookCaptureEvent);
        g_lookCaptureEvent=nullptr;
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        ReleaseSRWLockExclusive(&g_lookCaptureLock);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"breakpoint_install_failed"})";
    }

    InterlockedExchange(&g_lookCaptureArmed,1);
    ReleaseSRWLockExclusive(&g_lookCaptureLock);

    HANDLE thread=CreateThread(
        nullptr,0,&lookCaptureTimeoutThread,nullptr,0,nullptr);
    if (!thread) {
        AcquireSRWLockExclusive(&g_lookCaptureLock);
        restoreLookCaptureBreakpointLocked();
        if (g_lookVehHandle) {
            RemoveVectoredExceptionHandler(g_lookVehHandle);
            g_lookVehHandle=nullptr;
        }
        if (g_lookCaptureEvent) {
            CloseHandle(g_lookCaptureEvent);
            g_lookCaptureEvent=nullptr;
        }
        InterlockedExchange(&g_lookCaptureWorkerActive,0);
        ReleaseSRWLockExclusive(&g_lookCaptureLock);
        return R"({"ok":false,"cmd":"ARM_LOOK_ID","error":"timeout_thread_failed"})";
    }
    CloseHandle(thread);

    logLine("[LOOK] one-shot capture armed for 8s");
    return R"({"ok":true,"cmd":"ARM_LOOK_ID","armed":true,"timeoutMs":8000})";
}

void shutdownLookCapture() {
    HANDLE event{};
    AcquireSRWLockExclusive(&g_lookCaptureLock);
    restoreLookCaptureBreakpointLocked();
    event=g_lookCaptureEvent;
    if (event) SetEvent(event);
    ReleaseSRWLockExclusive(&g_lookCaptureLock);

    for (int i=0;i<100 &&
         InterlockedCompareExchange(&g_lookCaptureWorkerActive,0,0)!=0;
         ++i)
        Sleep(10);

    AcquireSRWLockExclusive(&g_lookCaptureLock);
    if (g_lookVehHandle) {
        RemoveVectoredExceptionHandler(g_lookVehHandle);
        g_lookVehHandle=nullptr;
    }
    if (g_lookCaptureEvent) {
        CloseHandle(g_lookCaptureEvent);
        g_lookCaptureEvent=nullptr;
    }
    InterlockedExchange(&g_lookCaptureWorkerActive,0);
    InterlockedExchange(&g_lookCaptureArmed,0);
    ReleaseSRWLockExclusive(&g_lookCaptureLock);
}

std::string getLookCaptureResponse() {
    const bool captured=
        InterlockedCompareExchange(&g_lookCaptureCaptured,0,0)!=0;
    const bool armed=
        InterlockedCompareExchange(&g_lookCaptureArmed,0,0)!=0;
    const bool busy=
        InterlockedCompareExchange(&g_lookCaptureWorkerActive,0,0)!=0;

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_LOOK_ID\""
      << ",\"armed\":" << (armed?"true":"false")
      << ",\"busy\":" << (busy?"true":"false")
      << ",\"captured\":" << (captured?"true":"false");

    if (captured) {
        o << ",\"id\":" << g_lookCapturedId
          << ",\"ageMs\":"
          << (GetTickCount64()>=g_lookCapturedTick
                ? GetTickCount64()-g_lookCapturedTick
                : 0);
    }

    o << "}";
    return o.str();
}

bool readMsvcString32(std::uintptr_t address, std::string& out) {
    out.clear();
    std::uint32_t len{}, cap{};
    if (!safeRead(address + 0x10, len) ||
        !safeRead(address + 0x14, cap) ||
        len > 96 || cap < len || cap > 0x10000)
        return false;

    if (len == 0) return true;

    char buf[97]{};
    if (cap < 16) {
        if (!safeCopyBytes(address, buf, len)) return false;
    } else {
        std::uint32_t ptr{};
        if (!safeRead(address, ptr) || !ptr ||
            !safeCopyBytes(static_cast<std::uintptr_t>(ptr), buf, len))
            return false;
    }

    for (std::uint32_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c < 0x20 || c > 0x7E) return false;
    }
    out.assign(buf, buf + len);
    return true;
}

struct ContainerItemSlot {
    std::uintptr_t holderAddress{};
    std::uintptr_t itemAddress{};
    std::uint16_t id{};
    std::uint16_t count{};
    std::uint32_t index{};
};

struct ContainerSnapshot {
    std::uintptr_t address{};
    std::uint32_t id{};
    std::uint32_t capacity{};
    std::string name;
    std::vector<ContainerItemSlot> items;
};

bool readContainerItems(std::uintptr_t container,
                        std::vector<ContainerItemSlot>& items) {
    items.clear();
    if (!g_profile || !g_profile->containerContractReady ||
        !exactObjectVtable(container, g_profile->containerVtableRva))
        return false;

    const auto base = container + g_profile->containerItemsOffset;
    std::uint32_t map{}, mapSize{}, myOff{}, mySize{};
    if (!safeRead(base + 0x0, map) ||
        !safeRead(base + 0x4, mapSize) ||
        !safeRead(base + 0x8, myOff) ||
        !safeRead(base + 0xC, mySize))
        return false;

    if (mySize == 0) return true;
    if (!map || !looksHeapPointer(map) ||
        mapSize == 0 || mapSize > 256 ||
        mySize > 100 ||
        g_profile->containerItemStride != 4)
        return false;

    constexpr std::uint32_t blockSize = 4; // MSVC x86 deque block for 4-byte holders.
    items.reserve(mySize);

    for (std::uint32_t i = 0; i < mySize; ++i) {
        const std::uint32_t logical = myOff + i;
        std::uint32_t blockIndex = logical / blockSize;
        if ((mapSize & (mapSize - 1)) == 0)
            blockIndex &= (mapSize - 1);
        else
            blockIndex %= mapSize;

        const std::uint32_t inner = logical % blockSize;
        std::uint32_t blockPtr{};
        if (!safeRead(static_cast<std::uintptr_t>(map) + blockIndex * 4u, blockPtr) ||
            !blockPtr || !looksHeapPointer(blockPtr))
            return false;

        const auto holder =
            static_cast<std::uintptr_t>(blockPtr) +
            inner * g_profile->containerItemStride;

        std::uintptr_t item{};
        if (!safeRead(holder, item) ||
            !item ||
            !exactObjectVtable(item, g_profile->itemVtableRva))
            return false;

        std::uint16_t id{}, count{};
        if (!safeRead(item + g_profile->itemIdOffset, id) || !id ||
            !safeRead(item + g_profile->itemCountOffset, count))
            return false;

        items.push_back({holder, item, id, count, i});
    }
    return true;
}

bool readContainerSnapshot(std::uintptr_t container, ContainerSnapshot& out) {
    out = {};
    if (!g_profile || !g_profile->containerContractReady ||
        !exactObjectVtable(container, g_profile->containerVtableRva))
        return false;

    std::uint32_t id{}, capacity{};
    if (!safeRead(container + g_profile->containerIdOffset, id) ||
        !safeRead(container + g_profile->containerCapacityOffset, capacity) ||
        id > 255 || capacity > 1000)
        return false;

    std::string name;
    readMsvcString32(container + g_profile->containerNameOffset, name);

    std::vector<ContainerItemSlot> items;
    if (!readContainerItems(container, items)) return false;
    if (capacity && items.size() > capacity) return false;

    out.address = container;
    out.id = id;
    out.capacity = capacity;
    out.name = std::move(name);
    out.items = std::move(items);
    return true;
}

std::vector<std::uintptr_t> discoverOpenContainerAddresses() {
    std::vector<std::uintptr_t> found;
    if (!g_profile || !g_profile->containerContractReady) return found;

    const auto game = g_base + g_profile->gameRva;
    const auto rootAddress = game + g_profile->openContainersOffset;

    struct Node { std::uintptr_t address{}; int depth{}; };
    std::vector<Node> queue;
    std::set<std::uintptr_t> seen;

    const auto scan = [&](auto&& self,
                          std::uintptr_t address,
                          std::size_t bytes,
                          int depth,
                          bool enqueue) -> void {
        for (std::size_t off = 0; off + 4 <= bytes; off += 4) {
            std::uint32_t value{};
            if (!safeRead(address + off, value) || !value) continue;
            const auto ptr = static_cast<std::uintptr_t>(value);

            if (exactObjectVtable(ptr, g_profile->containerVtableRva)) {
                found.push_back(ptr);
                continue;
            }

            if (enqueue && depth < 4 && queue.size() < 600 &&
                looksHeapPointer(ptr) && seen.insert(ptr).second) {
                queue.push_back({ptr, depth + 1});
            }
        }
    };

    // V25 profile evidence requires the first hop from Game+openContainers
    // to be exactly +0 of the inline registry root.
    scan(scan, rootAddress, 0x04, 0, true);
    for (std::size_t i = 0; i < queue.size() && i < 600; ++i)
        scan(scan, queue[i].address, 0xA0, queue[i].depth, true);

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

std::vector<ContainerSnapshot> scanOpenContainers() {
    std::vector<ContainerSnapshot> out;
    for (const auto address : discoverOpenContainerAddresses()) {
        ContainerSnapshot c;
        if (readContainerSnapshot(address, c))
            out.push_back(std::move(c));
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.id != b.id) return a.id < b.id;
        return a.address < b.address;
    });
    return out;
}

bool resolveTileItemHolder(std::uintptr_t tile,
                           std::uint16_t itemId,
                           std::uintptr_t expectedItem,
                           std::uintptr_t& holderOut,
                           std::uintptr_t& itemOut) {
    holderOut = itemOut = 0;
    if (!g_profile || !g_profile->itemContractReady || !tile || !itemId)
        return false;

    std::uint32_t begin{}, end{}, cap{}, count{};
    if (!readTileVectorHeader(tile, begin, end, cap, count) || !count)
        return false;

    std::uintptr_t fallbackHolder{}, fallbackItem{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto holder = static_cast<std::uintptr_t>(begin) + i * 4u;
        std::uintptr_t item{};
        if (!safeRead(holder, item) ||
            !item ||
            !exactObjectVtable(item, g_profile->itemVtableRva))
            continue;

        std::uint16_t id{};
        if (!safeRead(item + g_profile->itemIdOffset, id) || id != itemId)
            continue;

        if (expectedItem && item == expectedItem) {
            holderOut = holder;
            itemOut = item;
            return true;
        }
        if (!fallbackHolder) {
            fallbackHolder = holder;
            fallbackItem = item;
        }
    }

    holderOut = fallbackHolder;
    itemOut = fallbackItem;
    return holderOut != 0;
}

std::uint32_t tileSlotOf(const std::vector<std::uintptr_t>& tiles,
                         std::uintptr_t tile) {
    for (std::size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i] == tile) return static_cast<std::uint32_t>(i);
    return 0xFFFFFFFFu;
}

std::vector<TileItemDiagnostic> scanTileItemsFromSnapshot(
    const std::vector<std::uintptr_t>& tiles) {
    std::vector<TileItemDiagnostic> out;
    if (!g_profile || !g_profile->itemContractReady ||
        !g_profile->tileThingsRuntimeConfirmed)
        return out;

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

std::vector<TileMonsterDiagnostic> scanTileMonstersFromSnapshot(
    const std::vector<std::uintptr_t>& tiles,
    std::uint32_t maxRange = 15) {
    std::vector<TileMonsterDiagnostic> out;
    if (!g_profile || !g_profile->tileThingsRuntimeConfirmed)
        return out;

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz))
        return out;

    for (const auto tile : tiles) {
        std::vector<std::uintptr_t> things;
        bool containsLocalPlayer = false;
        if (!readStableTileThings(tile, things, containsLocalPlayer))
            continue;

        for (const auto thing : things) {
            VisibleMonster monster;
            if (!snapshotMonsterFromThing(
                    thing, px, py, pz, maxRange, monster))
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

std::vector<TileItemDiagnostic> scanCachedTileItems() {
    if (!g_profile || !g_profile->itemContractReady ||
        !g_profile->tileThingsRuntimeConfirmed)
        return {};

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz)) return {};
    if (!ensureTileCache(px, py, pz)) return {};

    return scanTileItemsFromSnapshot(copyTileCache());
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

    const auto cache = tileCacheInfo();
    const auto tiles = copyTileCache();
    const auto items = scanTileItemsFromSnapshot(tiles);
    const auto monsters = scanTileMonstersFromSnapshot(tiles, 15);

    AcquireSRWLockExclusive(&g_lootBaselineLock);
    g_lootBaseline.valid = true;
    g_lootBaseline.tileGeneration = cache.generation;
    g_lootBaseline.tiles = tiles;
    g_lootBaseline.items = items;
    g_lootBaseline.monsters = monsters;
    ReleaseSRWLockExclusive(&g_lootBaselineLock);

    AcquireSRWLockExclusive(&g_lootCandidateLock);
    g_lastLootCandidates.clear();
    ReleaseSRWLockExclusive(&g_lootCandidateLock);

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

std::string lootMarkTargetResponse(std::uint32_t targetId) {
    if (!targetId)
        return R"({"ok":false,"cmd":"LOOT_MARK_TARGET","error":"invalid_target_id"})";
    if (!g_profile || !g_profile->itemContractReady)
        return R"({"ok":false,"cmd":"LOOT_MARK_TARGET","error":"item_contract_not_ready"})";

    std::uint32_t px{}, py{};
    std::uint8_t pz{};
    if (!localPlayerPosition(px, py, pz))
        return R"({"ok":false,"cmd":"LOOT_MARK_TARGET","error":"player_unavailable"})";

    if (!ensureTileCache(px, py, pz))
        return R"({"ok":false,"cmd":"LOOT_MARK_TARGET","error":"tile_cache_unavailable"})";

    const auto cache = tileCacheInfo();
    const auto tiles = copyTileCache();
    const auto items = scanTileItemsFromSnapshot(tiles);
    const auto monsters = scanTileMonstersFromSnapshot(tiles, 15);

    const TileMonsterDiagnostic* tracked = nullptr;
    for (const auto& m : monsters) {
        if (m.monster.id == targetId) {
            tracked = &m;
            break;
        }
    }

    // Critical safety rule: if the requested target is already gone, DO NOT
    // overwrite the previous good baseline. That preserved baseline is the
    // last confirmed tile where the creature was alive.
    if (!tracked || tracked->monster.hpPercent == 0) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"LOOT_MARK_TARGET\","
          << "\"error\":\"target_not_alive\",\"targetId\":" << targetId << "}";
        return o.str();
    }

    std::vector<TileItemDiagnostic> targetTileItems;
    for (const auto& item : items) {
        if (item.tileAddress == tracked->tileAddress)
            targetTileItems.push_back(item);
    }

    AcquireSRWLockExclusive(&g_lootBaselineLock);

    // A different creature ID starts a fresh death-tile history. For the same
    // target, keep a short rolling history so a last-moment step cannot move
    // the corpse outside the single most recent sample.
    if (g_lootBaseline.trackedTargetId != targetId)
        g_lootBaseline.targetTileHistory.clear();

    LootTargetTileFrame frame;
    frame.tileAddress = tracked->tileAddress;
    frame.x = tracked->monster.x;
    frame.y = tracked->monster.y;
    frame.z = tracked->monster.z;
    frame.tick = GetTickCount64();
    frame.items = std::move(targetTileItems);

    if (!g_lootBaseline.targetTileHistory.empty() &&
        g_lootBaseline.targetTileHistory.back().tileAddress == frame.tileAddress) {
        g_lootBaseline.targetTileHistory.back() = std::move(frame);
    } else {
        g_lootBaseline.targetTileHistory.push_back(std::move(frame));
        constexpr std::size_t kLootTargetTileHistory = 6;
        if (g_lootBaseline.targetTileHistory.size() > kLootTargetTileHistory)
            g_lootBaseline.targetTileHistory.erase(
                g_lootBaseline.targetTileHistory.begin(),
                g_lootBaseline.targetTileHistory.begin() +
                (g_lootBaseline.targetTileHistory.size() - kLootTargetTileHistory));
    }

    g_lootBaseline.valid = true;
    g_lootBaseline.tileGeneration = cache.generation;
    g_lootBaseline.trackedTargetId = targetId;
    g_lootBaseline.trackedTargetTile = tracked->tileAddress;
    g_lootBaseline.tiles = tiles;
    g_lootBaseline.items = items;
    g_lootBaseline.monsters = monsters;
    const auto historyCount = g_lootBaseline.targetTileHistory.size();
    ReleaseSRWLockExclusive(&g_lootBaselineLock);

    AcquireSRWLockExclusive(&g_lootCandidateLock);
    g_lastLootCandidates.clear();
    ReleaseSRWLockExclusive(&g_lootCandidateLock);

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"LOOT_MARK_TARGET\","
      << "\"targetId\":" << targetId
      << ",\"targetName\":\"" << jsonEscape(tracked->monster.name) << "\""
      << ",\"targetTileSlot\":" << tileSlotOf(tiles, tracked->tileAddress)
      << ",\"targetTileHistory\":" << historyCount
      << ",\"tileGeneration\":" << cache.generation
      << ",\"baselineItems\":" << items.size()
      << "}";
    return o.str();
}

std::string lootDiffResponse() {
    LootBaselineState baseline;
    AcquireSRWLockShared(&g_lootBaselineLock);
    baseline = g_lootBaseline;
    ReleaseSRWLockShared(&g_lootBaselineLock);

    if (!baseline.valid)
        return R"({"ok":false,"cmd":"LOOT_DIFF","error":"no_baseline","hint":"run_LOOT_MARK_first"})";

    if (baseline.tiles.empty())
        return R"({"ok":false,"cmd":"LOOT_DIFF","error":"baseline_tile_snapshot_empty"})";

    const auto items = scanTileItemsFromSnapshot(baseline.tiles);
    const auto monsters = scanTileMonstersFromSnapshot(baseline.tiles, 15);
    const auto cache = tileCacheInfo();
    const auto& tiles = baseline.tiles;

    std::vector<TileItemDiagnostic> added;
    for (const auto& now : items) {
        bool existed = false;
        for (const auto& old : baseline.items) {
            if (sameItemIdentity(now, old)) { existed = true; break; }
        }
        if (!existed) added.push_back(now);
    }

    std::vector<TileMonsterDiagnostic> removed;
    for (const auto& oldMonster : baseline.monsters) {
        if (!currentMonsterContainsId(monsters, oldMonster.monster.id))
            removed.push_back(oldMonster);
    }

    std::vector<TileItemDiagnostic> deathTileCandidates;
    if (baseline.trackedTargetId && baseline.trackedTargetTile) {
        bool trackedRemoved = false;
        for (const auto& oldMonster : removed) {
            if (oldMonster.monster.id == baseline.trackedTargetId) {
                trackedRemoved = true;
                break;
            }
        }

        if (trackedRemoved) {
            auto pushCandidateUnique=[](
                std::vector<TileItemDiagnostic>& dst,
                const TileItemDiagnostic& item) {
                for (const auto& existing : dst) {
                    if (sameItemIdentity(existing,item))
                        return;
                }
                dst.push_back(item);
            };

            // Compare each recently confirmed target tile against the exact
            // item identities that existed there while the target was alive.
            // Decorative corpses/objects were already present in frame.items,
            // therefore they are never emitted as "new".
            for (auto fit = baseline.targetTileHistory.rbegin();
                 fit != baseline.targetTileHistory.rend(); ++fit) {

                const auto nowOnTile =
                    scanTileItemsFromSnapshot({fit->tileAddress});

                for (const auto& nowItem : nowOnTile) {
                    bool existed = false;
                    for (const auto& oldItem : fit->items) {
                        if (sameItemIdentity(nowItem,oldItem)) {
                            existed = true;
                            break;
                        }
                    }
                    if (!existed)
                        pushCandidateUnique(deathTileCandidates,nowItem);
                }
            }
        }
    } else if (!removed.empty()) {
        // Legacy LOOT_MARK diagnostic behavior only.
        deathTileCandidates = added;
    }

    AcquireSRWLockExclusive(&g_lootCandidateLock);
    g_lastLootCandidates = deathTileCandidates;
    ReleaseSRWLockExclusive(&g_lootCandidateLock);

    const auto& emittedItems =
        baseline.trackedTargetId ? deathTileCandidates : added;

    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"LOOT_DIFF\",\"readOnly\":true,"
      << "\"baselineGeneration\":" << baseline.tileGeneration
      << ",\"currentGlobalGeneration\":" << cache.generation
      << ",\"snapshotTiles\":" << baseline.tiles.size()
      << ",\"trackedTargetId\":" << baseline.trackedTargetId
      << ",\"trackedTargetTileSlot\":"
      << (baseline.trackedTargetTile
            ? tileSlotOf(tiles, baseline.trackedTargetTile)
            : 0xFFFFFFFFu)
      << ",\"trackedTargetTileHistory\":" << baseline.targetTileHistory.size()
      << ",\"candidatePolicy\":\"rolling_tracked_target_death_tiles\","
      << "\"addedCount\":" << emittedItems.size()
      << ",\"addedItems\":[";

    for (std::size_t i = 0; i < emittedItems.size() && i < 64; ++i) {
        if (i) o << ",";
        const auto& item = emittedItems[i];

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
    for (const auto& oldMonster : removed) {
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

    // IMPORTANT: Game::attack receives CreaturePtr BY VALUE on this OTC family.
    // shared_object_ptr is 4 bytes, so the actual stack value looks exactly like
    // a raw Creature*. The semantic difference is ownership: the normal caller
    // copy-constructs CreaturePtr (add_ref), and the callee destroys that
    // by-value parameter (dec_ref) on return. Calling with raw pointer bits
    // without the matching add_ref under-counts references and leaves
    // m_attackingCreature with invalid ownership after creatures disappear.
    const auto refAddress = target + p.sharedObjectRefCountOffset;
    std::uint32_t refsBefore{};
    if (!p.attackCreatureSharedValueConfirmed ||
        !p.sharedObjectRefCountOffset ||
        !safeRead(refAddress, refsBefore) ||
        refsBefore == 0 || refsBefore > 1000000u ||
        !isWritableAddress(refAddress)) {
        InterlockedExchange(&g_lastActionCode, 22);
        logLine("[TARGET TRACE] refusing attack: invalid shared_object refcount");
        return false;
    }

    const LONG refsAfterAdd =
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(refAddress));

    logLine("[TARGET TRACE] before Game::attack id=" + std::to_string(id) +
            " refsBefore=" + std::to_string(refsBefore) +
            " refsAfterAdd=" + std::to_string(refsAfterAdd));

    // Stack ABI remains pointer-sized: the single px field of CreaturePtr is
    // passed by value. The explicit add_ref above supplies the ownership step
    // that the original C++ caller performs before entering Game::attack.
    fn(reinterpret_cast<void*>(game), reinterpret_cast<void*>(target), false);

    std::uint32_t refsAfter{};
    safeRead(refAddress, refsAfter);
    logLine("[TARGET TRACE] Game::attack returned id=" + std::to_string(id) +
            " refsAfter=" + std::to_string(refsAfter));

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


bool executeUseOnGameThread() {
    InterlockedExchange(&g_lastActionCode, 0);

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode, 5);
        return false;
    }

    const auto& p = *g_profile;
    if (!p.useRuntimeConfirmed || !p.useItemSharedRefConfirmed ||
        !p.useMethodRva || !p.itemContractReady) {
        InterlockedExchange(&g_lastActionCode, 14);
        return false;
    }

    std::uintptr_t holder{}, item{};

    if (g_pendingUseContainer) {
        ContainerSnapshot source;
        if (!readContainerSnapshot(g_pendingUseContainer, source) ||
            source.id != g_pendingUseContainerId) {
            InterlockedExchange(&g_lastActionCode, 21);
            return false;
        }

        const ContainerItemSlot* sourceSlot = nullptr;
        for (const auto& slot : source.items) {
            if (slot.holderAddress == g_pendingUseHolder &&
                slot.itemAddress == g_pendingUseItem &&
                slot.id == g_pendingUseItemId) {
                sourceSlot = &slot;
                break;
            }
        }
        if (!sourceSlot) {
            InterlockedExchange(&g_lastActionCode, 21);
            return false;
        }

        holder = sourceSlot->holderAddress;
        item = sourceSlot->itemAddress;
    } else {
        if (!resolveTileItemHolder(g_pendingUseTile,
                                   g_pendingUseItemId,
                                   g_pendingUseItem,
                                   holder,
                                   item) ||
            !holder || !item) {
            InterlockedExchange(&g_lastActionCode, 11);
            return false;
        }
    }

    if (holder != g_pendingUseHolder || item != g_pendingUseItem) {
        InterlockedExchange(&g_lastActionCode, 12);
        return false;
    }

    const auto functionAddress = g_base + p.useMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode, 13);
        return false;
    }

    const auto game = g_base + p.gameRva;
    const auto fn = reinterpret_cast<GameUseFn>(functionAddress);

    logLine("[ACTION] USE_ITEM game-thread itemId=" +
            std::to_string(static_cast<unsigned>(g_pendingUseItemId)));

    // Runtime ABI capture says Game::use takes Item shared_object_ptr_ref.
    // The 4-byte Tile::things element is the actual holder; pass its address,
    // never the raw Item*.
    fn(reinterpret_cast<void*>(game),
       reinterpret_cast<void*>(holder));

    InterlockedExchange(&g_lastActionCode, 1);
    return true;
}

bool executeUseWithSelfOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode,5);
        return false;
    }

    const auto& p=*g_profile;
    if (!p.useWithRuntimeConfirmed ||
        !p.useWithItemSharedRefConfirmed ||
        !p.useWithThingSharedRefConfirmed ||
        !p.useWithMethodRva ||
        !p.containerContractReady ||
        !p.itemContractReady ||
        !p.localPlayerVtableRva) {
        InterlockedExchange(&g_lastActionCode,26);
        return false;
    }

    ContainerSnapshot source;
    if (!readContainerSnapshot(g_pendingUseContainer,source) ||
        source.id!=g_pendingUseContainerId) {
        InterlockedExchange(&g_lastActionCode,21);
        return false;
    }

    const ContainerItemSlot* sourceSlot=nullptr;
    for (const auto& slot:source.items) {
        if (slot.holderAddress==g_pendingUseHolder &&
            slot.itemAddress==g_pendingUseItem &&
            slot.id==g_pendingUseItemId) {
            sourceSlot=&slot;
            break;
        }
    }
    if (!sourceSlot) {
        InterlockedExchange(&g_lastActionCode,21);
        return false;
    }

    const auto game=g_base+p.gameRva;
    std::uintptr_t localPlayer{};
    if (!safeRead(game+p.localPlayerOffset,localPlayer) || !localPlayer ||
        !exactObjectVtable(localPlayer,p.localPlayerVtableRva)) {
        InterlockedExchange(&g_lastActionCode,27);
        return false;
    }

    const auto functionAddress=g_base+p.useWithMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode,28);
        return false;
    }

    // Game::useWith(const ItemPtr&, const ThingPtr&).
    // arg0 is the real holder from Container::items. For self, construct the
    // pointer-sized ThingPtr view expected by the const-ref ABI and keep it
    // alive for the complete native call. The LocalPlayer object itself is
    // still owned by Game::m_localPlayer.
    std::uintptr_t selfThingHolder=localPlayer;

    const auto fn=reinterpret_cast<GameUseWithFn>(functionAddress);

    logLine("[ACTION] USE_WITH_SELF itemId=" +
            std::to_string(static_cast<unsigned>(g_pendingUseItemId)) +
            " sourceContainer=" +
            std::to_string(g_pendingUseContainerId));

    fn(reinterpret_cast<void*>(game),
       reinterpret_cast<void*>(sourceSlot->holderAddress),
       reinterpret_cast<void*>(&selfThingHolder));

    InterlockedExchange(&g_lastActionCode,1);
    return true;
}

bool executeMoveOnGameThread() {
    InterlockedExchange(&g_lastActionCode, 0);

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode, 5);
        return false;
    }

    const auto& p = *g_profile;
    if (!p.moveRuntimeConfirmed || !p.moveItemSharedRefConfirmed ||
        !p.movePositionRefConfirmed || !p.moveCountConfirmed ||
        !p.moveMethodRva || !p.containerContractReady) {
        InterlockedExchange(&g_lastActionCode, 18);
        return false;
    }

    ContainerSnapshot source;
    if (!readContainerSnapshot(g_pendingMoveSourceContainer, source)) {
        InterlockedExchange(&g_lastActionCode, 15);
        return false;
    }

    const ContainerItemSlot* sourceSlot = nullptr;
    for (const auto& slot : source.items) {
        if (slot.holderAddress == g_pendingMoveHolder &&
            slot.itemAddress == g_pendingMoveItem &&
            slot.id == g_pendingMoveItemId) {
            sourceSlot = &slot;
            break;
        }
    }
    if (!sourceSlot) {
        InterlockedExchange(&g_lastActionCode, 15);
        return false;
    }

    ContainerSnapshot target;
    if (!readContainerSnapshot(g_pendingMoveTargetContainer, target) ||
        target.id != g_pendingMoveTargetContainerId) {
        InterlockedExchange(&g_lastActionCode, 16);
        return false;
    }

    if (target.capacity && g_pendingMoveTargetSlot >= target.capacity) {
        InterlockedExchange(&g_lastActionCode, 19);
        return false;
    }

    const auto functionAddress = g_base + p.moveMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode, 17);
        return false;
    }

    // OTClient container position convention. Live V15 validation confirmed
    // that x=0xFFFF, y=0x40|containerId, z=slot moves the requested stack
    // into the open destination container on this Ramonia build.
    MovePosition dest{};
    dest.x = 0xFFFFu;
    dest.y = 0x40u | (g_pendingMoveTargetContainerId & 0xFFu);
    dest.z = g_pendingMoveTargetSlot;

    std::uint32_t moveCount = g_pendingMoveCount;
    if (!moveCount) moveCount = sourceSlot->count ? sourceSlot->count : 1u;

    const auto game = g_base + p.gameRva;
    const auto fn = reinterpret_cast<GameMoveFn>(functionAddress);

    logLine("[ACTION] MOVE_ITEM game-thread itemId=" +
            std::to_string(static_cast<unsigned>(g_pendingMoveItemId)) +
            " count=" + std::to_string(moveCount) +
            " targetContainer=" + std::to_string(g_pendingMoveTargetContainerId) +
            " targetSlot=" + std::to_string(static_cast<unsigned>(g_pendingMoveTargetSlot)));

    // Runtime ABI capture says arg1 is shared_object_ptr_ref. Container::items
    // is a deque of 4-byte holders, so the deque element address is the
    // reference object expected by Game::move.
    fn(reinterpret_cast<void*>(game),
       reinterpret_cast<void*>(sourceSlot->holderAddress),
       &dest,
       moveCount);

    InterlockedExchange(&g_lastActionCode, 1);
    return true;
}

bool executeTalkOnGameThread() {
    InterlockedExchange(&g_lastActionCode,0);

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode,5);
        return false;
    }

    const auto& p=*g_profile;
    if (!p.talkChannelRuntimeConfirmed ||
        !p.talkModeArgConfirmed ||
        !p.talkChannelArgConfirmed ||
        !p.talkMessageMsvcStringRefConfirmed ||
        !p.talkChannelMethodRva) {
        InterlockedExchange(&g_lastActionCode,23);
        return false;
    }

    const std::size_t len=std::strlen(g_pendingTalkMessage);
    if (!len || len>15) {
        InterlockedExchange(&g_lastActionCode,24);
        return false;
    }

    MsvcSmallString32 message;
    std::memcpy(message.storage,g_pendingTalkMessage,len);
    message.storage[len]='\0';
    message.size=static_cast<std::uint32_t>(len);
    message.capacity=15;

    const auto functionAddress=g_base+p.talkChannelMethodRva;
    if (!isExecutableAddress(functionAddress)) {
        InterlockedExchange(&g_lastActionCode,25);
        return false;
    }

    const auto game=g_base+p.gameRva;
    const auto fn=reinterpret_cast<GameTalkChannelFn>(functionAddress);

    logLine("[ACTION] TALK_CHANNEL mode=" +
            std::to_string(g_pendingTalkMode) +
            " channel=" + std::to_string(g_pendingTalkChannel) +
            " message=" + std::string(g_pendingTalkMessage));

    fn(reinterpret_cast<void*>(game),
       g_pendingTalkMode,
       g_pendingTalkChannel,
       &message);

    InterlockedExchange(&g_lastActionCode,1);
    return true;
}

LRESULT CALLBACK gameThreadGetMessageHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && lParam) {
        auto* msg = reinterpret_cast<MSG*>(lParam);
        if (msg->message == kActionMessage) {
            const LONG token = static_cast<LONG>(msg->wParam);
            const LONG expected =
                InterlockedCompareExchange(&g_pendingActionToken, 0, 0);

            if (token != 0 && token == expected) {
                const LONG kind =
                    InterlockedCompareExchange(&g_pendingActionKind, 0, 0);

                if (kind == PendingAttack)
                    executeAttackSelectedOnGameThread();
                else if (kind == PendingUse)
                    executeUseOnGameThread();
                else if (kind == PendingMove)
                    executeMoveOnGameThread();
                else if (kind == PendingTalk)
                    executeTalkOnGameThread();
                else if (kind == PendingUseWithSelf)
                    executeUseWithSelfOnGameThread();
                else
                    InterlockedExchange(&g_lastActionCode, 7);

                // Keep the V12 safety property: preserve the hook chain and
                // remove this hook immediately after the one action.
                const HHOOK hook = g_gameThreadHook;
                const LRESULT next = CallNextHookEx(hook, code, wParam, lParam);
                if (hook) {
                    UnhookWindowsHookEx(hook);
                    if (g_gameThreadHook == hook) g_gameThreadHook = nullptr;
                }
                logLine("[TARGET TRACE] hook removed after action");
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
    // Re-resolve the current game window/thread for every on-demand action.
    // Do not carry a cached thread id across map/floor transitions.
    const HWND currentWindow = findCurrentProcessWindow();
    if (!currentWindow) {
        logLine("[DISPATCH] main game window not found");
        g_gameWindow = nullptr;
        g_gameThreadId = 0;
        return false;
    }

    const DWORD currentThreadId =
        GetWindowThreadProcessId(currentWindow, nullptr);
    if (!currentThreadId) {
        logLine("[DISPATCH] game window thread id unavailable");
        g_gameWindow = nullptr;
        g_gameThreadId = 0;
        return false;
    }

    const bool changed =
        currentWindow != g_gameWindow ||
        currentThreadId != g_gameThreadId;

    g_gameWindow = currentWindow;
    g_gameThreadId = currentThreadId;

    if (changed) {
        logLine("[DISPATCH] on-demand game-thread target refreshed tid=" +
                std::to_string(g_gameThreadId));
    }
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

bool dispatchPendingActionToGameThread(DWORD& win32Error) {
    win32Error = ERROR_SUCCESS;

    if (!prepareGameThreadDispatcher()) {
        win32Error = ERROR_INVALID_WINDOW_HANDLE;
        InterlockedExchange(&g_pendingActionKind, PendingNone);
        return false;
    }

    // V12: no hook/event remains installed while the bridge is idle.
    cleanupOnDemandDispatcher();

    g_actionEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_actionEvent) {
        win32Error = GetLastError();
        InterlockedExchange(&g_pendingActionKind, PendingNone);
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
        InterlockedExchange(&g_pendingActionKind, PendingNone);
        cleanupOnDemandDispatcher();
        return false;
    }

    logLine("[TARGET TRACE] hook installed tid=" +
            std::to_string(g_gameThreadId));

    ResetEvent(g_actionEvent);
    InterlockedExchange(&g_lastActionCode, 0);

    LONG token = InterlockedIncrement(&g_actionTokenCounter);
    if (token == 0) token = InterlockedIncrement(&g_actionTokenCounter);
    InterlockedExchange(&g_pendingActionToken, token);

    SetLastError(0);
    if (!PostThreadMessageW(
            g_gameThreadId,
            kActionMessage,
            static_cast<WPARAM>(static_cast<ULONG_PTR>(token)),
            0)) {
        win32Error = GetLastError();
        InterlockedCompareExchange(&g_pendingActionToken, 0, token);
        InterlockedExchange(&g_pendingActionKind, PendingNone);
        cleanupOnDemandDispatcher();
        return false;
    }

    const DWORD wait = WaitForSingleObject(g_actionEvent, 3000);
    if (wait != WAIT_OBJECT_0) {
        win32Error = (wait == WAIT_TIMEOUT) ? ERROR_TIMEOUT : GetLastError();
        InterlockedCompareExchange(&g_pendingActionToken, 0, token);
        InterlockedExchange(&g_pendingActionKind, PendingNone);
        cleanupOnDemandDispatcher();
        return false;
    }

    InterlockedCompareExchange(&g_pendingActionToken, 0, token);

    // The hook normally removes itself in the callback. This is only a safety
    // cleanup for unexpected paths.
    cleanupOnDemandDispatcher();
    InterlockedExchange(&g_pendingActionKind, PendingNone);
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
        case 11: return "loot_item_not_visible";
        case 12: return "item_holder_changed";
        case 13: return "use_function_not_executable";
        case 14: return "use_contract_not_ready";
        case 15: return "move_source_invalid";
        case 16: return "move_target_invalid";
        case 17: return "move_function_not_executable";
        case 18: return "move_contract_not_ready";
        case 19: return "move_target_full";
        case 20: return "loot_candidate_missing";
        case 21: return "container_item_not_visible";
        case 22: return "attack_shared_object_refcount_invalid";
        case 23: return "talk_contract_not_ready";
        case 24: return "talk_message_invalid";
        case 25: return "talk_function_not_executable";
        case 26: return "usewith_self_contract_not_ready";
        case 27: return "local_player_holder_invalid";
        case 28: return "usewith_function_not_executable";
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

std::optional<std::uint32_t> uint32FieldAllowZeroFromRequest(
    const std::string& request, const char* field) {
    try {
        const auto root = crome::json::parse(request);
        if (!root.isObject()) return std::nullopt;
        const auto n = integerValue(root.get(field));
        if (!n || *n > 0xFFFFFFFFull) return std::nullopt;
        return static_cast<std::uint32_t>(*n);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> stringFieldFromRequest(
    const std::string& request,const char* field) {
    try {
        const auto root=crome::json::parse(request);
        if (!root.isObject()) return std::nullopt;
        const auto* v=root.get(field);
        if (!v || !v->isString()) return std::nullopt;
        return v->string();
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
    double hp{}, maxHp{}, mana{}, maxMana{}, soul{};
    const bool ok =
        safeRead(localPlayer + p.posX, x) &&
        safeRead(localPlayer + p.posY, y) &&
        safeRead(localPlayer + p.posZ, z) &&
        readNumber(localPlayer + p.health.offset, p.health.type, hp) &&
        readNumber(localPlayer + p.maxHealth.offset, p.maxHealth.type, maxHp) &&
        readNumber(localPlayer + p.mana.offset, p.mana.type, mana) &&
        readNumber(localPlayer + p.maxMana.offset, p.maxMana.type, maxMana);

    if (!ok) return R"({"ok":false,"cmd":"GET_PLAYER","error":"snapshot_read_failed"})";

    const bool soulAvailable =
        p.soul.valid &&
        readNumber(localPlayer + p.soul.offset,p.soul.type,soul) &&
        std::isfinite(soul) &&
        soul >= 0.0;

    std::ostringstream o;
    o << std::fixed << std::setprecision(2)
      << "{\"ok\":true,\"cmd\":\"GET_PLAYER\",\"player\":{"
      << "\"health\":" << hp << ",\"maxHealth\":" << maxHp
      << ",\"mana\":" << mana << ",\"maxMana\":" << maxMana
      << ",\"soulAvailable\":" << (soulAvailable ? "true" : "false");

    if (soulAvailable)
        o << ",\"soul\":" << soul;

    o << ",\"position\":{\"x\":" << x << ",\"y\":" << y << ",\"z\":"
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


std::string openContainersResponse() {
    if (!g_profile || !g_profile->containerContractReady)
        return R"({"ok":false,"cmd":"GET_OPEN_CONTAINERS","error":"container_contract_not_ready"})";

    const auto containers = scanOpenContainers();
    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":\"GET_OPEN_CONTAINERS\",\"count\":"
      << containers.size() << ",\"containers\":[";

    for (std::size_t i = 0; i < containers.size() && i < 32; ++i) {
        if (i) o << ",";
        const auto& c = containers[i];
        o << "{\"id\":" << c.id
          << ",\"name\":\"" << jsonEscape(c.name) << "\""
          << ",\"capacity\":" << c.capacity
          << ",\"itemCount\":" << c.items.size()
          << ",\"items\":[";

        for (std::size_t j = 0; j < c.items.size() && j < 100; ++j) {
            if (j) o << ",";
            const auto& item = c.items[j];
            o << "{\"slot\":" << item.index
              << ",\"id\":" << item.id
              << ",\"count\":" << item.count << "}";
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

std::string useLootItemResponse(std::uint16_t itemId) {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"build_not_validated"})";
    if (!g_profile->useRuntimeConfirmed ||
        !g_profile->useItemSharedRefConfirmed ||
        !g_profile->itemContractReady)
        return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"use_contract_not_ready"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"dispatcher_not_ready"})";

    std::vector<TileItemDiagnostic> candidates;
    AcquireSRWLockShared(&g_lootCandidateLock);
    candidates = g_lastLootCandidates;
    ReleaseSRWLockShared(&g_lootCandidateLock);

    std::optional<TileItemDiagnostic> chosen;
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        if (it->id == itemId) {
            chosen = *it;
            break;
        }
    }
    if (!chosen)
        return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"loot_candidate_missing","hint":"run_LOOT_MARK_then_LOOT_DIFF"})";

    std::uintptr_t holder{}, item{};
    if (!resolveTileItemHolder(chosen->tileAddress,
                               itemId,
                               chosen->address,
                               holder,
                               item) ||
        !holder || !item)
        return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"loot_item_not_visible"})";

    const auto before = scanOpenContainers();

    g_pendingUseTile = chosen->tileAddress;
    g_pendingUseContainer = 0;
    g_pendingUseContainerId = 0;
    g_pendingUseHolder = holder;
    g_pendingUseItem = item;
    g_pendingUseItemId = itemId;
    InterlockedExchange(&g_pendingActionKind, PendingUse);

    DWORD win32{};
    const bool dispatched = dispatchPendingActionToGameThread(win32);
    const LONG code = InterlockedCompareExchange(&g_lastActionCode, 0, 0);

    g_pendingUseTile = 0;
    g_pendingUseContainer = 0;
    g_pendingUseContainerId = 0;
    g_pendingUseHolder = 0;
    g_pendingUseItem = 0;
    g_pendingUseItemId = 0;

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"USE_LOOT_ITEM\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    Sleep(250);
    const auto after = scanOpenContainers();

    std::ostringstream o;
    o << "{\"ok\":" << (code == 1 ? "true" : "false")
      << ",\"cmd\":\"USE_LOOT_ITEM\""
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"itemId\":" << itemId
      << ",\"tileSlot\":" << chosen->tileSlot
      << ",\"openContainersBefore\":" << before.size()
      << ",\"openContainersAfter\":" << after.size()
      << ",\"holderAbi\":\"shared_object_ptr_ref\"}";
    return o.str();
}

std::string useContainerItemResponse(
    const std::optional<std::uint32_t>& sourceContainerId,
    std::uint16_t itemId) {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"build_not_validated"})";
    if (!g_profile->useRuntimeConfirmed ||
        !g_profile->useItemSharedRefConfirmed ||
        !g_profile->itemContractReady ||
        !g_profile->containerContractReady)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"use_contract_not_ready"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"dispatcher_not_ready"})";

    const auto containers = scanOpenContainers();

    const ContainerSnapshot* source = nullptr;
    const ContainerItemSlot* sourceSlot = nullptr;
    bool requestedContainerOpen = !sourceContainerId.has_value();

    for (const auto& container : containers) {
        if (sourceContainerId && container.id != *sourceContainerId)
            continue;

        if (sourceContainerId && container.id == *sourceContainerId)
            requestedContainerOpen = true;

        for (const auto& slot : container.items) {
            if (slot.id == itemId) {
                source = &container;
                sourceSlot = &slot;
                break;
            }
        }
        if (sourceSlot) break;
    }

    if (!requestedContainerOpen)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"source_container_not_open"})";
    if (!source || !sourceSlot)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"item_not_found_in_open_containers"})";

    g_pendingUseTile = 0;
    g_pendingUseContainer = source->address;
    g_pendingUseContainerId = source->id;
    g_pendingUseHolder = sourceSlot->holderAddress;
    g_pendingUseItem = sourceSlot->itemAddress;
    g_pendingUseItemId = sourceSlot->id;
    InterlockedExchange(&g_pendingActionKind, PendingUse);

    DWORD win32{};
    const bool dispatched = dispatchPendingActionToGameThread(win32);
    const LONG code = InterlockedCompareExchange(&g_lastActionCode, 0, 0);

    const auto usedContainerId = source->id;
    const auto usedSlot = sourceSlot->index;
    const auto usedCount = sourceSlot->count;

    g_pendingUseTile = 0;
    g_pendingUseContainer = 0;
    g_pendingUseContainerId = 0;
    g_pendingUseHolder = 0;
    g_pendingUseItem = 0;
    g_pendingUseItemId = 0;

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"USE_CONTAINER_ITEM\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    std::ostringstream o;
    o << "{\"ok\":" << (code == 1 ? "true" : "false")
      << ",\"cmd\":\"USE_CONTAINER_ITEM\""
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"sourceContainerId\":" << usedContainerId
      << ",\"slot\":" << usedSlot
      << ",\"itemId\":" << itemId
      << ",\"count\":" << usedCount
      << ",\"holderAbi\":\"shared_object_ptr_ref\""
      << ",\"source\":\"open_container\"}";
    return o.str();
}

std::string useContainerItemOnSelfResponse(
    const std::optional<std::uint32_t>& sourceContainerId,
    std::uint16_t itemId) {

    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"build_not_validated"})";
    if (!g_profile->useWithRuntimeConfirmed ||
        !g_profile->useWithItemSharedRefConfirmed ||
        !g_profile->useWithThingSharedRefConfirmed ||
        !g_profile->containerContractReady ||
        !g_profile->itemContractReady ||
        !g_profile->localPlayerVtableRva)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"usewith_self_contract_not_ready"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"dispatcher_not_ready"})";

    const auto containers=scanOpenContainers();

    const ContainerSnapshot* source=nullptr;
    const ContainerItemSlot* sourceSlot=nullptr;
    bool requestedContainerOpen=!sourceContainerId.has_value();

    for (const auto& container:containers) {
        if (sourceContainerId && container.id!=*sourceContainerId)
            continue;

        if (sourceContainerId && container.id==*sourceContainerId)
            requestedContainerOpen=true;

        for (const auto& slot:container.items) {
            if (slot.id==itemId) {
                source=&container;
                sourceSlot=&slot;
                break;
            }
        }
        if (sourceSlot) break;
    }

    if (!requestedContainerOpen)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"source_container_not_open"})";
    if (!source || !sourceSlot)
        return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"item_not_found_in_open_containers"})";

    g_pendingUseTile=0;
    g_pendingUseContainer=source->address;
    g_pendingUseContainerId=source->id;
    g_pendingUseHolder=sourceSlot->holderAddress;
    g_pendingUseItem=sourceSlot->itemAddress;
    g_pendingUseItemId=sourceSlot->id;
    InterlockedExchange(&g_pendingActionKind,PendingUseWithSelf);

    DWORD win32{};
    const bool dispatched=dispatchPendingActionToGameThread(win32);
    const LONG code=InterlockedCompareExchange(&g_lastActionCode,0,0);

    const auto usedContainerId=source->id;
    const auto usedSlot=sourceSlot->index;
    const auto usedCount=sourceSlot->count;

    g_pendingUseTile=0;
    g_pendingUseContainer=0;
    g_pendingUseContainerId=0;
    g_pendingUseHolder=0;
    g_pendingUseItem=0;
    g_pendingUseItemId=0;

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"USE_CONTAINER_ITEM_ON_SELF\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    std::ostringstream o;
    o << "{\"ok\":" << (code==1?"true":"false")
      << ",\"cmd\":\"USE_CONTAINER_ITEM_ON_SELF\""
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"sourceContainerId\":" << usedContainerId
      << ",\"slot\":" << usedSlot
      << ",\"itemId\":" << itemId
      << ",\"count\":" << usedCount
      << ",\"itemHolderAbi\":\"shared_object_ptr_ref\""
      << ",\"targetHolderAbi\":\"shared_object_ptr_ref\""
      << ",\"target\":\"self\"}";
    return o.str();
}

std::string moveContainerItemResponse(std::uint32_t sourceContainerId,
                                      std::uint32_t targetContainerId,
                                      std::uint16_t itemId) {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"build_not_validated"})";
    if (!g_profile->moveRuntimeConfirmed ||
        !g_profile->moveItemSharedRefConfirmed ||
        !g_profile->movePositionRefConfirmed ||
        !g_profile->moveCountConfirmed ||
        !g_profile->containerContractReady)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"move_contract_not_ready"})";
    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"dispatcher_not_ready"})";

    const auto containers = scanOpenContainers();

    const ContainerSnapshot* source = nullptr;
    const ContainerSnapshot* target = nullptr;
    for (const auto& c : containers) {
        if (c.id == sourceContainerId && !source) source = &c;
        if (c.id == targetContainerId && !target) target = &c;
    }

    if (!source)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"source_container_not_open"})";
    if (!target)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"target_container_not_open"})";
    if (sourceContainerId == targetContainerId)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"source_equals_target"})";

    const ContainerItemSlot* sourceSlot = nullptr;
    for (const auto& item : source->items) {
        if (item.id == itemId) {
            sourceSlot = &item;
            break;
        }
    }
    if (!sourceSlot)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"item_not_found_in_source"})";

    const std::uint32_t targetSlot =
        static_cast<std::uint32_t>(target->items.size());
    if (target->capacity && targetSlot >= target->capacity)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"target_container_full"})";
    if (targetSlot > 255u)
        return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"target_slot_out_of_range"})";

    g_pendingMoveSourceContainer = source->address;
    g_pendingMoveHolder = sourceSlot->holderAddress;
    g_pendingMoveItem = sourceSlot->itemAddress;
    g_pendingMoveItemId = sourceSlot->id;
    g_pendingMoveCount = sourceSlot->count ? sourceSlot->count : 1u;
    g_pendingMoveTargetContainer = target->address;
    g_pendingMoveTargetContainerId = target->id;
    g_pendingMoveTargetSlot = static_cast<std::uint8_t>(targetSlot);
    InterlockedExchange(&g_pendingActionKind, PendingMove);

    DWORD win32{};
    const bool dispatched = dispatchPendingActionToGameThread(win32);
    const LONG code = InterlockedCompareExchange(&g_lastActionCode, 0, 0);

    const auto movedCount = g_pendingMoveCount;
    const auto destinationSlot = g_pendingMoveTargetSlot;

    g_pendingMoveSourceContainer = 0;
    g_pendingMoveHolder = 0;
    g_pendingMoveItem = 0;
    g_pendingMoveItemId = 0;
    g_pendingMoveCount = 0;
    g_pendingMoveTargetContainer = 0;
    g_pendingMoveTargetContainerId = 0;
    g_pendingMoveTargetSlot = 0;

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"MOVE_CONTAINER_ITEM\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    std::ostringstream o;
    o << "{\"ok\":" << (code == 1 ? "true" : "false")
      << ",\"cmd\":\"MOVE_CONTAINER_ITEM\""
      << ",\"result\":\"" << actionCodeText(code) << "\""
      << ",\"sourceContainerId\":" << sourceContainerId
      << ",\"targetContainerId\":" << targetContainerId
      << ",\"itemId\":" << itemId
      << ",\"count\":" << movedCount
      << ",\"destinationSlot\":" << static_cast<unsigned>(destinationSlot)
      << ",\"holderAbi\":\"shared_object_ptr_ref\""
      << ",\"destinationEncoding\":\"otclient_container_position_runtime_confirmed\"}";
    return o.str();
}

std::string attackIdResponse(std::uint32_t creatureId) {
    logLine("[TARGET TRACE] ATTACK_ID request id=" + std::to_string(creatureId));
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

    InterlockedExchange(&g_pendingActionKind, PendingAttack);
    DWORD win32{};
    const bool dispatched = dispatchPendingActionToGameThread(win32);

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

    InterlockedExchange(&g_pendingActionKind, PendingAttack);
    DWORD win32{};
    const bool dispatched = dispatchPendingActionToGameThread(win32);
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
std::string sayResponse(const std::string& message) {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"SAY","error":"build_not_validated"})";
    if (message.empty() || message.size()>15)
        return R"({"ok":false,"cmd":"SAY","error":"message_must_be_1_to_15_ascii_chars"})";

    for (const unsigned char c:message) {
        if (c<0x20 || c>0x7E)
            return R"({"ok":false,"cmd":"SAY","error":"message_ascii_only"})";
    }

    if (!prepareGameThreadDispatcher())
        return R"({"ok":false,"cmd":"SAY","error":"dispatcher_not_ready"})";

    std::memset(g_pendingTalkMessage,0,sizeof(g_pendingTalkMessage));
    std::memcpy(g_pendingTalkMessage,message.data(),message.size());

    // OTC-family Game::talk is talkChannel(MessageSay,0,message).
    // MessageSay=1 is cross-checked against upstream OTClient/OTCv8 source;
    // the Ramonia talkChannel ABI itself is runtime-confirmed in this profile.
    g_pendingTalkMode=g_profile->talkSayModeValue;
    g_pendingTalkChannel=g_profile->talkDefaultChannel;
    InterlockedExchange(&g_pendingActionKind,PendingTalk);

    DWORD win32{};
    const bool dispatched=dispatchPendingActionToGameThread(win32);
    const LONG code=InterlockedCompareExchange(&g_lastActionCode,0,0);

    std::memset(g_pendingTalkMessage,0,sizeof(g_pendingTalkMessage));

    if (!dispatched) {
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"SAY\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << win32 << "}";
        return o.str();
    }

    std::ostringstream o;
    o << "{\"ok\":" << (code==1?"true":"false")
      << ",\"cmd\":\"SAY\",\"result\":\"" << actionCodeText(code)
      << "\",\"mode\":" << g_profile->talkSayModeValue
      << ",\"channel\":" << g_profile->talkDefaultChannel
      << ",\"message\":\"" << jsonEscape(message) << "\"}";
    return o.str();
}

std::string handleRequest(const std::string& request) {
    const auto cmd = commandFromRequest(request);
    if (cmd == "HELLO") {
        std::ostringstream o;
        o << "{\"ok\":true,\"cmd\":\"HELLO\",\"bridge\":\"CromeRamoniaBridge\","
          << "\"bridgeVersion\":26,\"protocol\":1,\"pid\":" << GetCurrentProcessId()
          << ",\"pipe\":\"" << jsonEscape(pipeName()) << "\","
          << "\"status\":\"" << jsonEscape(g_status) << "\","
          << "\"profileReady\":" << (g_profile.has_value() ? "true" : "false")
          << ",\"buildMatched\":" << (g_buildMatched ? "true" : "false")
          << ",\"lookReady\":"
          << ((g_profile && g_profile->lookRuntimeConfirmed &&
               g_profile->lookItemSharedRefConfirmed) ? "true" : "false")
          << ",\"lightReady\":"
          << ((g_profile && g_profile->worldLightContractReady) ? "true" : "false")
          << ",\"lightEnabled\":" << (g_worldLightEnabled ? "true" : "false")
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
    if (cmd == "ARM_LOOK_ID") return armLookCaptureResponse();
    if (cmd == "GET_LOOK_ID") return getLookCaptureResponse();
    if (cmd == "SAY") {
        const auto message=stringFieldFromRequest(request,"message");
        if (!message)
            return R"({"ok":false,"cmd":"SAY","error":"invalid_message"})";
        return sayResponse(*message);
    }
    if (cmd == "GET_LIGHT") return worldLightStatusResponse("GET_LIGHT");
    if (cmd == "SET_LIGHT") {
        const auto enabled=uint32FieldAllowZeroFromRequest(request,"enabled");
        if (!enabled || *enabled > 1u)
            return R"({"ok":false,"cmd":"SET_LIGHT","error":"invalid_enabled"})";
        return setWorldLightResponse(*enabled != 0u);
    }
    if (cmd == "GET_COMBAT_TARGETS") return combatTargetsResponse(p);
    if (cmd == "GET_VISIBLE_MONSTERS") return visibleMonstersResponse();
    if (cmd == "GET_TILE_MONSTERS") return tileMonstersResponse();
    if (cmd == "LOOT_MARK") return lootMarkResponse();
    if (cmd == "LOOT_MARK_TARGET") {
        const auto id = uint32FieldFromRequest(request, "creatureId");
        if (!id || !*id)
            return R"({"ok":false,"cmd":"LOOT_MARK_TARGET","error":"invalid_creature_id"})";
        return lootMarkTargetResponse(*id);
    }
    if (cmd == "LOOT_DIFF") return lootDiffResponse();
    if (cmd == "GET_OPEN_CONTAINERS") return openContainersResponse();
    if (cmd == "USE_LOOT_ITEM") {
        const auto id = uint32FieldFromRequest(request, "itemId");
        if (!id || *id > 0xFFFFu)
            return R"({"ok":false,"cmd":"USE_LOOT_ITEM","error":"invalid_item_id"})";
        return useLootItemResponse(static_cast<std::uint16_t>(*id));
    }
    if (cmd == "USE_CONTAINER_ITEM_ON_SELF") {
        const auto itemId=uint32FieldFromRequest(request,"itemId");
        if (!itemId || !*itemId || *itemId>0xFFFFu)
            return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM_ON_SELF","error":"invalid_item_id"})";

        const auto sourceContainerId=
            uint32FieldAllowZeroFromRequest(request,"sourceContainerId");
        return useContainerItemOnSelfResponse(
            sourceContainerId,
            static_cast<std::uint16_t>(*itemId));
    }

    if (cmd == "USE_CONTAINER_ITEM") {
        const auto item = uint32FieldFromRequest(request, "itemId");
        if (!item || *item > 0xFFFFu)
            return R"({"ok":false,"cmd":"USE_CONTAINER_ITEM","error":"invalid_item_id"})";

        // sourceContainerId is optional. When omitted, search all currently
        // open containers and use the first matching item.
        const auto sourceId =
            uint32FieldAllowZeroFromRequest(request, "sourceContainerId");

        return useContainerItemResponse(
            sourceId, static_cast<std::uint16_t>(*item));
    }
    if (cmd == "MOVE_CONTAINER_ITEM") {
        const auto sourceId =
            uint32FieldAllowZeroFromRequest(request, "sourceContainerId");
        const auto targetId =
            uint32FieldAllowZeroFromRequest(request, "targetContainerId");
        const auto item =
            uint32FieldFromRequest(request, "itemId");
        if (!sourceId || !targetId || !item || *item > 0xFFFFu)
            return R"({"ok":false,"cmd":"MOVE_CONTAINER_ITEM","error":"invalid_arguments"})";
        return moveContainerItemResponse(
            *sourceId, *targetId, static_cast<std::uint16_t>(*item));
    }
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
            if (clear) clear << "CromeRamoniaBridge V26 startup\r\n";
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

    shutdownLookCapture();
    if (g_worldLightEnabled)
        restoreWorldLightHack();
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
