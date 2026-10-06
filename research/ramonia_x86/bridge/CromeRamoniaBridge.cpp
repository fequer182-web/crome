#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MiniJson.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

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
WNDPROC g_originalWndProc{};
constexpr UINT kAttackSelectedMessage = WM_APP + 0x620;

volatile LONG g_lastActionCode{};
std::uint32_t g_lastActionTargetId{};

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
    std::uintptr_t monsterVtableRva{};
    std::uintptr_t attackMethodRva{};
    bool attackRuntimeConfirmed{};

    struct Field {
        std::size_t offset{};
        std::string type;
        bool valid{};
    };
    Field health, maxHealth, mana, maxMana;
};

std::optional<Profile> g_profile;

std::string moduleDirectory();

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

    const auto* actions = root.get("actions");
    const auto* attack = child(actions, "attack");
    if (attack && attack->isObject()) {
        if (const auto n = integerValue(attack->get("methodRva")))
            p.attackMethodRva = static_cast<std::uintptr_t>(*n);
        p.attackRuntimeConfirmed = stringValue(attack->get("status")) == "runtime_confirmed";
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

    const auto* fields = child(runtime, "selectedFields");
    p.health = readField(fields, "health");
    p.maxHealth = readField(fields, "maxHealth");
    p.mana = readField(fields, "mana");
    p.maxMana = readField(fields, "maxMana");
    if (!p.health.valid || !p.maxHealth.valid || !p.mana.valid || !p.maxMana.valid)
        throw std::runtime_error("required player fields missing");
    if (!p.expectedTimestamp || !p.expectedImageSize)
        throw std::runtime_error("build fingerprint incomplete");
    if (!p.monsterVtableRva || !p.creatureIdOffset || !p.attackMethodRva ||
        !p.attackRuntimeConfirmed)
        throw std::runtime_error("V37 attack contract incomplete");

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

bool executeAttackSelectedOnGameThread() {
    InterlockedExchange(&g_lastActionCode, 0);
    g_lastActionTargetId = 0;

    if (!g_profile || !g_buildMatched) {
        InterlockedExchange(&g_lastActionCode, 5);
        return false;
    }

    const auto& p = *g_profile;
    const auto game = g_base + p.gameRva;

    std::uintptr_t followTarget{};
    std::uintptr_t attackTarget{};
    safeRead(game + p.followTargetOffset, followTarget);
    safeRead(game + p.attackTargetOffset, attackTarget);

    // For the first active proof, the user selects a Monster with Follow.
    // If Follow is empty, reuse the current AttackTarget only as a fallback.
    const auto target = followTarget ? followTarget : attackTarget;
    if (!target) {
        InterlockedExchange(&g_lastActionCode, 2);
        return false;
    }

    std::uint32_t id{};
    if (!targetSnapshot(target, id)) {
        InterlockedExchange(&g_lastActionCode, 3);
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

LRESULT CALLBACK bridgeWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kAttackSelectedMessage) {
        return executeAttackSelectedOnGameThread() ? 1 : 0;
    }
    return g_originalWndProc
        ? CallWindowProc(g_originalWndProc, hwnd, message, wParam, lParam)
        : DefWindowProc(hwnd, message, wParam, lParam);
}

bool installGameThreadDispatcher() {
    if (g_gameWindow && g_originalWndProc) return true;
    g_gameWindow = findCurrentProcessWindow();
    if (!g_gameWindow) {
        logLine("[DISPATCH] main game window not found");
        return false;
    }

    SetLastError(0);
    const LONG_PTR previous = SetWindowLongPtr(
        g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&bridgeWndProc));
    if (!previous) {
        const auto error = GetLastError();
        logLine("[DISPATCH] SetWindowLongPtr failed Win32=" + std::to_string(error));
        g_gameWindow = nullptr;
        return false;
    }

    g_originalWndProc = reinterpret_cast<WNDPROC>(previous);
    logLine("[DISPATCH] game-thread dispatcher installed");
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

std::string attackSelectedResponse() {
    if (!g_profile || !g_buildMatched)
        return R"({"ok":false,"cmd":"ATTACK_SELECTED","error":"build_not_validated"})";

    if (!g_gameWindow || !g_originalWndProc)
        return R"({"ok":false,"cmd":"ATTACK_SELECTED","error":"dispatcher_not_ready"})";

    InterlockedExchange(&g_lastActionCode, 0);
    g_lastActionTargetId = 0;

    DWORD_PTR dispatchResult{};
    SetLastError(0);
    const auto sent = SendMessageTimeout(
        g_gameWindow,
        kAttackSelectedMessage,
        0,
        0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        3000,
        &dispatchResult);

    if (!sent) {
        InterlockedExchange(&g_lastActionCode, 8);
        std::ostringstream o;
        o << "{\"ok\":false,\"cmd\":\"ATTACK_SELECTED\","
          << "\"error\":\"dispatcher_timeout\",\"win32\":" << GetLastError() << "}";
        return o.str();
    }

    const LONG code = InterlockedCompareExchange(&g_lastActionCode, 0, 0);
    std::ostringstream o;
    o << "{\"ok\":" << (code == 1 ? "true" : "false")
      << ",\"cmd\":\"ATTACK_SELECTED\",\"result\":\"" << actionCodeText(code)
      << "\",\"targetId\":" << g_lastActionTargetId
      << ",\"gameThreadResult\":" << static_cast<unsigned long long>(dispatchResult) << "}";
    return o.str();
}

std::string handleRequest(const std::string& request) {
    const auto cmd = commandFromRequest(request);
    if (cmd == "HELLO") {
        std::ostringstream o;
        o << "{\"ok\":true,\"cmd\":\"HELLO\",\"bridge\":\"CromeRamoniaBridge\","
          << "\"bridgeVersion\":4,\"protocol\":1,\"pid\":" << GetCurrentProcessId()
          << ",\"pipe\":\"" << jsonEscape(pipeName()) << "\","
          << "\"status\":\"" << jsonEscape(g_status) << "\","
          << "\"profileReady\":" << (g_profile.has_value() ? "true" : "false")
          << ",\"buildMatched\":" << (g_buildMatched ? "true" : "false")
          << ",\"dispatcherReady\":" << ((g_gameWindow && g_originalWndProc) ? "true" : "false");
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
    if (cmd == "ATTACK_SELECTED") return attackSelectedResponse();
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
            if (clear) clear << "CromeRamoniaBridge V2 startup\r\n";
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

            if (!installGameThreadDispatcher())
                throw std::runtime_error("game-thread dispatcher install failed");

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
