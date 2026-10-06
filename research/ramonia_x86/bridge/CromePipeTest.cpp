#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include "MiniJson.hpp"

#include <cstdint>
#include <cctype>
#include <cwctype>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <optional>
#include <chrono>
#include <algorithm>

namespace {

DWORD findRamoniaPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring exe = pe.szExeFile;
            for (auto& c : exe) c = static_cast<wchar_t>(towlower(c));
            if (exe.rfind(L"ramonia_gl-", 0) == 0 && exe.size() >= 4 &&
                exe.substr(exe.size() - 4) == L".exe") {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

bool request(DWORD pid, const std::string& json) {
    const std::string pipeName = "\\\\.\\pipe\\Crome.Ramonia." + std::to_string(pid);

    if (!WaitNamedPipeA(pipeName.c_str(), 2500)) {
        std::cout << "[ERRO] Pipe nao encontrado: " << pipeName
                  << " Win32=" << GetLastError() << "\n";
        return false;
    }

    HANDLE pipe = CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        std::cout << "[ERRO] CreateFile pipe Win32=" << GetLastError() << "\n";
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    DWORD written{};
    if (!WriteFile(pipe, json.data(), static_cast<DWORD>(json.size()), &written, nullptr)) {
        std::cout << "[ERRO] WriteFile Win32=" << GetLastError() << "\n";
        CloseHandle(pipe);
        return false;
    }

    char buffer[8192]{};
    DWORD read{};
    const BOOL ok = ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr);
    if (!ok && GetLastError() != ERROR_MORE_DATA) {
        std::cout << "[ERRO] ReadFile Win32=" << GetLastError() << "\n";
        CloseHandle(pipe);
        return false;
    }

    const std::string response(buffer, buffer + read);
    std::cout << response << "\n";
    CloseHandle(pipe);

    if (response.find("\"ok\":false") != std::string::npos) {
        std::cout << "[ERRO] A bridge respondeu, mas o comando falhou.\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    DWORD pid = 0;
    std::string mode = "baseline";
    std::uint32_t creatureId = 0;

    if (argc >= 2) {
        const std::string first = argv[1];
        try {
            std::size_t used{};
            const auto parsed = std::stoul(first, &used, 10);
            if (used == first.size()) pid = static_cast<DWORD>(parsed);
            else mode = first;
        } catch (...) {
            mode = first;
        }
    }

    for (auto& c : mode)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (mode == "attackid") {
        if (argc >= 3) {
            try { creatureId = static_cast<std::uint32_t>(std::stoul(argv[2])); }
            catch (...) {}
        }
        if (argc >= 4) {
            try { pid = static_cast<DWORD>(std::stoul(argv[3])); }
            catch (...) {}
        }
    } else if (argc >= 3 && !pid) {
        try { pid = static_cast<DWORD>(std::stoul(argv[2])); }
        catch (...) {}
    }

    if (!pid) pid = findRamoniaPid();

    if (!pid) {
        std::cout << "[ERRO] Ramonia nao encontrado.\n";
        return 1;
    }

    std::cout << "============================================================\n"
              << "       Crome Pipe Test - external x64 -> Ramonia x86\n"
              << "============================================================\n"
              << "PID  : " << pid << "\n"
              << "PIPE : \\\\.\\pipe\\Crome.Ramonia." << pid << "\n\n";

    bool ok = true;

    if (mode == "monsters") {
        std::cout << "[GET_VISIBLE_MONSTERS]\n";
        ok &= request(pid, R"({"cmd":"GET_VISIBLE_MONSTERS"})");
        std::cout << "\n" << (ok ? "[OK] Monsters visiveis lidos sem Follow manual.\n"
                                   : "[FALHOU] Nao foi possivel listar os monsters.\n");
        return ok ? 0 : 2;
    }

    if (mode == "attackid") {
        if (!creatureId) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe attackid <creatureId> [PID]\n";
            return 1;
        }

        std::cout << "[GET_COMBAT_TARGETS - BEFORE]\n";
        ok &= request(pid, R"({"cmd":"GET_COMBAT_TARGETS"})");
        if (!ok) return 2;

        std::cout << "\n[ATTACK_ID]\n";
        const std::string attack =
            "{\"cmd\":\"ATTACK_ID\",\"creatureId\":" + std::to_string(creatureId) + "}";
        ok &= request(pid, attack);

        std::cout << "\n[GET_COMBAT_TARGETS - AFTER]\n";
        ok &= request(pid, R"({"cmd":"GET_COMBAT_TARGETS"})");

        std::cout << "\n" << (ok ? "[OK] ATTACK_ID executado sem Follow manual.\n"
                                   : "[FALHOU] ATTACK_ID nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "targets") {
        std::cout << "[GET_COMBAT_TARGETS]\n";
        ok &= request(pid, R"({"cmd":"GET_COMBAT_TARGETS"})");
        std::cout << "\n" << (ok ? "[OK] Combat targets lidos.\n"
                                   : "[FALHOU] Nao foi possivel ler combat targets.\n");
        return ok ? 0 : 2;
    }

    if (mode == "attack") {
        std::cout << "[GET_COMBAT_TARGETS - BEFORE]\n";
        ok &= request(pid, R"({"cmd":"GET_COMBAT_TARGETS"})");
        if (!ok) return 2;

        std::cout << "\n[ATTACK_SELECTED]\n";
        ok &= request(pid, R"({"cmd":"ATTACK_SELECTED"})");

        std::cout << "\n[GET_COMBAT_TARGETS - AFTER]\n";
        ok &= request(pid, R"({"cmd":"GET_COMBAT_TARGETS"})");

        std::cout << "\n" << (ok ? "[OK] ATTACK_SELECTED executado no game thread.\n"
                                   : "[FALHOU] ATTACK_SELECTED nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    std::cout << "[HELLO]\n";
    ok &= request(pid, R"({"cmd":"HELLO"})");

    std::cout << "\n[BUILD_INFO]\n";
    ok &= request(pid, R"({"cmd":"BUILD_INFO"})");

    std::cout << "\n[GET_PLAYER]\n";
    ok &= request(pid, R"({"cmd":"GET_PLAYER"})");

    std::cout << "\n" << (ok ? "[OK] Pipe + profile + snapshot funcionando.\n"
                               : "[FALHOU] Veja a primeira mensagem de erro acima.\n");
    return ok ? 0 : 2;
}
