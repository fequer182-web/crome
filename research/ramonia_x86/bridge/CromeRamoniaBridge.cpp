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

HMODULE g_module{};
volatile LONG g_running = 1;

struct Profile {
    std::string schema;
    int profileVersion{};
    std::string architecture;
    std::string timestamp;
    std::string sizeOfImage;
    std::string textHash;
    std::uintptr_t gameRva{};
    std::size_t localPlayerOffset{};
    std::size_t posX{}, posY{}, posZ{};

    struct Field {
        std::size_t offset{};
        std::string type;
        bool valid{};
    };
    Field health, maxHealth, mana, maxMana;
};

std::string readText(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open profile: " + path.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string moduleDirectory() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().string();
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

Profile loadProfile(const std::filesystem::path& path) {
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
    }

    const auto* owners = root.get("owners");
    const auto game = integerValue(child(owners, "gameRva"));
    if (!game) throw std::runtime_error("owners.gameRva missing");
    p.gameRva = static_cast<std::uintptr_t>(*game);

    const auto* layout = root.get("gameLayout");
    const auto lp = integerValue(child(layout, "localPlayerOffset"));
    if (!lp) throw std::runtime_error("gameLayout.localPlayerOffset missing");
    p.localPlayerOffset = static_cast<std::size_t>(*lp);

    const auto* runtime = root.get("runtime");
    const auto* position = child(runtime, "position");
    const auto x = integerValue(child(position, "x"));
    const auto y = integerValue(child(position, "y"));
    const auto z = integerValue(child(position, "z"));
    if (!x || !y || !z) throw std::runtime_error("runtime.position incomplete");
    p.posX = static_cast<std::size_t>(*x);
    p.posY = static_cast<std::size_t>(*y);
    p.posZ = static_cast<std::size_t>(*z);

    const auto* fields = child(runtime, "selectedFields");
    p.health = readField(fields, "health");
    p.maxHealth = readField(fields, "maxHealth");
    p.mana = readField(fields, "mana");
    p.maxMana = readField(fields, "maxMana");
    if (!p.health.valid || !p.maxHealth.valid || !p.mana.valid || !p.maxMana.valid)
        throw std::runtime_error("required player fields missing");

    return p;
}

std::filesystem::path findProfilePath() {
    const auto dir = std::filesystem::path(moduleDirectory()) / "profiles" / "ramonia_x86";
    if (!std::filesystem::exists(dir)) throw std::runtime_error("profiles\\ramonia_x86 not found");
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".json") return e.path();
    }
    throw std::runtime_error("Ramonia profile JSON not found");
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

std::string handleRequest(const std::string& request, const Profile& p, std::uintptr_t base) {
    const auto cmd = commandFromRequest(request);
    if (cmd == "HELLO") {
        std::ostringstream o;
        o << "{\"ok\":true,\"cmd\":\"HELLO\",\"bridge\":\"CromeRamoniaBridge\","
          << "\"protocol\":1,\"pid\":" << GetCurrentProcessId()
          << ",\"pipe\":\"" << jsonEscape(pipeName()) << "\"}";
        return o.str();
    }
    if (cmd == "BUILD_INFO") {
        std::ostringstream o;
        o << "{\"ok\":true,\"cmd\":\"BUILD_INFO\",\"schema\":\"" << jsonEscape(p.schema)
          << "\",\"profileVersion\":" << p.profileVersion
          << ",\"architecture\":\"" << jsonEscape(p.architecture)
          << "\",\"timestamp\":\"" << jsonEscape(p.timestamp)
          << "\",\"sizeOfImage\":\"" << jsonEscape(p.sizeOfImage)
          << "\",\"textHash\":\"" << jsonEscape(p.textHash) << "\"}";
        return o.str();
    }
    if (cmd == "GET_PLAYER") return playerResponse(p, base);
    return "{\"ok\":false,\"error\":\"unknown_command\",\"cmd\":\"" + jsonEscape(cmd) + "\"}";
}

void serveOne(const Profile& profile, std::uintptr_t base) {
    const auto name = pipeName();
    HANDLE pipe = CreateNamedPipeA(
        name.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 8192, 8192, 1000, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        Sleep(500);
        return;
    }

    const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE :
        (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

    if (connected) {
        char buffer[4096]{};
        DWORD read{};
        if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr) && read) {
            std::string request(buffer, buffer + read);
            const auto response = handleRequest(request, profile, base);
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
        const auto profilePath = findProfilePath();
        const auto profile = loadProfile(profilePath);
        if (profile.architecture != "x86")
            throw std::runtime_error("profile architecture is not x86");

        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        while (InterlockedCompareExchange(&g_running, 1, 1))
            serveOne(profile, base);
    } catch (...) {
        // Keep DllMain-safe behavior: failure simply leaves no pipe endpoint.
    }
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        InterlockedExchange(&g_running, 1);
        if (HANDLE h = CreateThread(nullptr, 0, bridgeThread, nullptr, 0, nullptr))
            CloseHandle(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_running, 0);
    }
    return TRUE;
}
