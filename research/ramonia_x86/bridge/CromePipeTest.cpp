#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cwctype>
#include <iostream>
#include <string>

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

    std::cout << std::string(buffer, buffer + read) << "\n";
    CloseHandle(pipe);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    DWORD pid = 0;
    if (argc >= 2) {
        try { pid = static_cast<DWORD>(std::stoul(argv[1])); }
        catch (...) {}
    }
    if (!pid) pid = findRamoniaPid();

    if (!pid) {
        std::cout << "[ERRO] Ramonia nao encontrado. Rode com: CromePipeTest_x64.exe <PID>\n";
        return 1;
    }

    std::cout << "============================================================\n"
              << "       Crome Pipe Test - external x64 -> Ramonia x86\n"
              << "============================================================\n"
              << "PID  : " << pid << "\n"
              << "PIPE : \\\\.\\pipe\\Crome.Ramonia." << pid << "\n\n";

    bool ok = true;
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
