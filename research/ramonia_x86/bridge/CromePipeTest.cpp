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

bool requestRaw(DWORD pid, const std::string& json, std::string& response, bool printResponse = true) {
    const std::string pipeName = "\\\\.\\pipe\\Crome.Ramonia." + std::to_string(pid);

    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD lastError = ERROR_SUCCESS;

    // The bridge serves one request per pipe instance. Between Disconnect/Close
    // and the next CreateNamedPipe there is a tiny window where Windows reports
    // FILE_NOT_FOUND instead of waiting. Retry that transport-only race locally.
    for (int attempt = 0; attempt < 20; ++attempt) {
        pipe = CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;

        lastError = GetLastError();
        if (lastError != ERROR_PIPE_BUSY && lastError != ERROR_FILE_NOT_FOUND) break;

        if (lastError == ERROR_PIPE_BUSY)
            WaitNamedPipeA(pipeName.c_str(), 100);

        Sleep(25);
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        if (printResponse)
            std::cout << "[ERRO] Pipe indisponivel: " << pipeName
                      << " Win32=" << lastError << "\n";
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    DWORD written{};
    if (!WriteFile(pipe, json.data(), static_cast<DWORD>(json.size()), &written, nullptr)) {
        if (printResponse)
            std::cout << "[ERRO] WriteFile Win32=" << GetLastError() << "\n";
        CloseHandle(pipe);
        return false;
    }

    char buffer[8192]{};
    DWORD read{};
    const BOOL ok = ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr);
    if (!ok && GetLastError() != ERROR_MORE_DATA) {
        if (printResponse)
            std::cout << "[ERRO] ReadFile Win32=" << GetLastError() << "\n";
        CloseHandle(pipe);
        return false;
    }

    response.assign(buffer, buffer + read);
    CloseHandle(pipe);

    if (printResponse) std::cout << response << "\n";

    if (response.find("\"ok\":false") != std::string::npos) {
        if (printResponse)
            std::cout << "[ERRO] A bridge respondeu, mas o comando falhou.\n";
        return false;
    }
    return true;
}

bool request(DWORD pid, const std::string& json) {
    std::string response;
    return requestRaw(pid, json, response, true);
}

struct MonsterView {
    std::uint32_t id{};
    std::string name;
    std::uint32_t hpPercent{};
    std::uint32_t distance{};
};

std::string lowerCopy(std::string value) {
    for (auto& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::vector<MonsterView> parseVisibleMonsters(const std::string& json) {
    std::vector<MonsterView> out;
    try {
        const auto root = crome::json::parse(json);
        const auto* monsters = root.get("monsters");
        if (!monsters || !monsters->isArray()) return out;

        for (const auto& v : monsters->array()) {
            if (!v.isObject()) continue;
            const auto* id = v.get("id");
            const auto* name = v.get("name");
            const auto* hp = v.get("hpPercent");
            const auto* distance = v.get("distance");
            if (!id || !id->isNumber() || !name || !name->isString() ||
                !hp || !hp->isNumber() || !distance || !distance->isNumber())
                continue;

            MonsterView m;
            m.id = static_cast<std::uint32_t>(id->number());
            m.name = name->string();
            m.hpPercent = static_cast<std::uint32_t>(hp->number());
            m.distance = static_cast<std::uint32_t>(distance->number());
            if (m.id && m.hpPercent <= 100) out.push_back(std::move(m));
        }
    } catch (...) {}
    return out;
}

std::uint32_t parseAttackTargetId(const std::string& json) {
    try {
        const auto root = crome::json::parse(json);
        const auto* attack = root.get("attack");
        if (!attack || !attack->isObject()) return 0;
        const auto* id = attack->get("id");
        return id && id->isNumber() ? static_cast<std::uint32_t>(id->number()) : 0;
    } catch (...) {
        return 0;
    }
}

std::optional<MonsterView> chooseTarget(
    const std::vector<MonsterView>& monsters,
    const std::string& wantedName,
    std::uint32_t currentAttackId) {

    const auto wanted = lowerCopy(wantedName);
    const bool wildcard = wanted == "*";

    // Keep the current target while it is still alive and matches the rule.
    for (const auto& m : monsters) {
        if (m.id != currentAttackId || m.hpPercent == 0) continue;
        if (wildcard || lowerCopy(m.name) == wanted) return m;
    }

    std::optional<MonsterView> best;
    for (const auto& m : monsters) {
        if (m.hpPercent == 0) continue;
        if (!wildcard && lowerCopy(m.name) != wanted) continue;
        if (!best || m.distance < best->distance) best = m;
    }
    return best;
}

int runTargetLoop(DWORD pid, const std::string& wantedName) {
    std::cout << "[TARGET] Ativo para: " << wantedName << "\n"
              << "[TARGET] CTRL+C para encerrar.\n\n";

    std::uint32_t lastTargetId = 0;
    std::uint32_t lastHp = 101;
    bool waitingShown = false;

    for (;;) {
        std::string monstersJson;
        if (!requestRaw(pid, R"({"cmd":"GET_VISIBLE_MONSTERS"})", monstersJson, false)) {
            std::cout << "[TARGET] Falha ao ler monsters; tentando novamente...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        std::string combatJson;
        if (!requestRaw(pid, R"({"cmd":"GET_COMBAT_TARGETS"})", combatJson, false)) {
            std::cout << "[TARGET] Falha ao ler combat target; tentando novamente...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        const auto monsters = parseVisibleMonsters(monstersJson);
        const auto currentAttackId = parseAttackTargetId(combatJson);
        const auto chosen = chooseTarget(monsters, wantedName, currentAttackId);

        if (!chosen) {
            if (!waitingShown) {
                std::cout << "[TARGET] Nenhum alvo correspondente visivel. Aguardando...\n";
                waitingShown = true;
            }
            lastTargetId = 0;
            lastHp = 101;
            std::this_thread::sleep_for(std::chrono::milliseconds(750));
            continue;
        }

        waitingShown = false;

        if (chosen->id != currentAttackId) {
            const std::string cmd =
                "{\"cmd\":\"ATTACK_ID\",\"creatureId\":" +
                std::to_string(chosen->id) + "}";
            std::string attackResponse;
            std::cout << "[TARGET] Atacando " << chosen->name
                      << " id=" << chosen->id
                      << " hp=" << chosen->hpPercent << "%"
                      << " dist=" << chosen->distance << "\n";
            if (!requestRaw(pid, cmd, attackResponse, false)) {
                if (attackResponse.find("\"creature_id_not_visible\"") != std::string::npos) {
                    std::cout << "[TARGET] Alvo sumiu/morreu antes do ataque; procurando o proximo.\n";
                } else {
                    std::cout << "[TARGET] ATTACK_ID falhou; nova tentativa no proximo ciclo.\n";
                }
            }
        } else if (chosen->id != lastTargetId || chosen->hpPercent != lastHp) {
            std::cout << "[TARGET] " << chosen->name
                      << " id=" << chosen->id
                      << " hp=" << chosen->hpPercent << "%"
                      << " dist=" << chosen->distance << "\n";
        }

        lastTargetId = chosen->id;
        lastHp = chosen->hpPercent;
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
    }
}

} // namespace

int main(int argc, char** argv) {
    DWORD pid = 0;
    std::string mode = "baseline";
    std::uint32_t creatureId = 0;
    std::string wantedName;
    std::uint32_t lootItemId = 2813;
    std::uint32_t sourceContainerId = 0;
    std::uint32_t targetContainerId = 0;
    std::uint32_t moveItemId = 2148;
    std::uint32_t useContainerItemId = 2696;
    std::uint32_t useContainerId = 0;
    bool useContainerProvided = false;
    bool sourceContainerProvided = false;
    bool targetContainerProvided = false;

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
    } else if (mode == "target") {
        if (argc >= 3) wantedName = argv[2];
        if (argc >= 4) {
            try { pid = static_cast<DWORD>(std::stoul(argv[3])); }
            catch (...) {}
        }
    } else if (mode == "usecorpse" || mode == "useloot") {
        if (argc >= 3) {
            try { lootItemId = static_cast<std::uint32_t>(std::stoul(argv[2])); }
            catch (...) {}
        }
        if (argc >= 4) {
            try { pid = static_cast<DWORD>(std::stoul(argv[3])); }
            catch (...) {}
        }
    } else if (mode == "useitem" || mode == "usefood") {
        if (argc >= 3) {
            std::string containerArg = argv[2];
            for (auto& c : containerArg)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (containerArg != "any" && containerArg != "*") {
                try {
                    useContainerId = static_cast<std::uint32_t>(std::stoul(containerArg));
                    useContainerProvided = true;
                } catch (...) {}
            }
        }
        if (argc >= 4) {
            try { useContainerItemId = static_cast<std::uint32_t>(std::stoul(argv[3])); }
            catch (...) {}
        }
        if (argc >= 5) {
            try { pid = static_cast<DWORD>(std::stoul(argv[4])); }
            catch (...) {}
        }
    } else if (mode == "movegold" || mode == "moveitem") {
        if (argc >= 3) {
            try {
                sourceContainerId = static_cast<std::uint32_t>(std::stoul(argv[2]));
                sourceContainerProvided = true;
            } catch (...) {}
        }
        if (argc >= 4) {
            try {
                targetContainerId = static_cast<std::uint32_t>(std::stoul(argv[3]));
                targetContainerProvided = true;
            } catch (...) {}
        }
        if (argc >= 5) {
            try { moveItemId = static_cast<std::uint32_t>(std::stoul(argv[4])); }
            catch (...) {}
        }
        if (argc >= 6) {
            try { pid = static_cast<DWORD>(std::stoul(argv[5])); }
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

    if (mode == "target") {
        if (wantedName.empty()) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe target <nome|*> [PID]\n";
            return 1;
        }
        return runTargetLoop(pid, wantedName);
    }

    if (mode == "monsters") {
        std::cout << "[GET_VISIBLE_MONSTERS]\n";
        ok &= request(pid, R"({"cmd":"GET_VISIBLE_MONSTERS"})");
        std::cout << "\n" << (ok ? "[OK] Monsters visiveis lidos sem Follow manual.\n"
                                   : "[FALHOU] Nao foi possivel listar os monsters.\n");
        return ok ? 0 : 2;
    }

    if (mode == "tilemon" || mode == "tilemonsters") {
        std::cout << "[GET_TILE_MONSTERS - READ ONLY]\n";
        ok &= request(pid, R"({"cmd":"GET_TILE_MONSTERS"})");
        std::cout << "\n" << (ok ? "[OK] Tile::things lido sem ataque e sem Follow manual.\n"
                                   : "[FALHOU] Nao foi possivel ler Monsters via Tile::things.\n");
        return ok ? 0 : 2;
    }

    if (mode == "lootmark") {
        std::cout << "[LOOT_MARK - READ ONLY]\n";
        ok &= request(pid, R"({"cmd":"LOOT_MARK"})");
        std::cout << "\n" << (ok ? "[OK] Snapshot de Tile::things salvo. Agora mate UMA criatura sem andar longe.\n"
                                   : "[FALHOU] Nao foi possivel criar o snapshot de loot.\n");
        return ok ? 0 : 2;
    }

    if (mode == "lootdiff") {
        std::cout << "[LOOT_DIFF - READ ONLY]\n";
        ok &= request(pid, R"({"cmd":"LOOT_DIFF"})");
        std::cout << "\n" << (ok ? "[OK] Diferenca de Tile::things calculada.\n"
                                   : "[FALHOU] Nao foi possivel comparar o snapshot de loot.\n");
        return ok ? 0 : 2;
    }

    if (mode == "containers") {
        std::cout << "[GET_OPEN_CONTAINERS - READ ONLY]\n";
        ok &= request(pid, R"({"cmd":"GET_OPEN_CONTAINERS"})");
        std::cout << "\n" << (ok ? "[OK] Containers abertos e itens lidos.\n"
                                   : "[FALHOU] Nao foi possivel ler containers abertos.\n");
        return ok ? 0 : 2;
    }

    if (mode == "usecorpse" || mode == "useloot") {
        if (!lootItemId || lootItemId > 0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe usecorpse [itemId=2813] [PID]\n";
            return 1;
        }
        std::cout << "[USE_LOOT_ITEM - EXPERIMENTAL]\n";
        const std::string cmd =
            "{\"cmd\":\"USE_LOOT_ITEM\",\"itemId\":" +
            std::to_string(lootItemId) + "}";
        ok &= request(pid, cmd);
        std::cout << "\n" << (ok ? "[OK] Game::use foi chamado no holder do Item. Confira se o corpse abriu.\n"
                                   : "[FALHOU] USE_LOOT_ITEM nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "useitem" || mode == "usefood") {
        if (!useContainerItemId || useContainerItemId > 0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe usefood <containerId|any> [itemId=2696] [PID]\n";
            return 1;
        }

        std::cout << "[USE_CONTAINER_ITEM - EXPERIMENTAL]\n";
        std::string cmd = "{\"cmd\":\"USE_CONTAINER_ITEM\",\"itemId\":" +
                          std::to_string(useContainerItemId);
        if (useContainerProvided)
            cmd += ",\"sourceContainerId\":" + std::to_string(useContainerId);
        cmd += "}";

        ok &= request(pid, cmd);
        std::cout << "\n" << (ok
            ? "[OK] Game::use foi chamado no item do container. Confira se o food foi consumido.\n"
            : "[FALHOU] USE_CONTAINER_ITEM nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "movegold" || mode == "moveitem") {
        if (!sourceContainerProvided || !targetContainerProvided ||
            !moveItemId || moveItemId > 0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe movegold <corpseContainerId> <bagContainerId> [itemId=2148] [PID]\n";
            return 1;
        }
        std::cout << "[MOVE_CONTAINER_ITEM - EXPERIMENTAL]\n";
        const std::string cmd =
            "{\"cmd\":\"MOVE_CONTAINER_ITEM\",\"sourceContainerId\":" +
            std::to_string(sourceContainerId) +
            ",\"targetContainerId\":" + std::to_string(targetContainerId) +
            ",\"itemId\":" + std::to_string(moveItemId) + "}";
        ok &= request(pid, cmd);
        std::cout << "\n" << (ok ? "[OK] Game::move foi chamado. Confira o item na bag.\n"
                                   : "[FALHOU] MOVE_CONTAINER_ITEM nao foi confirmado.\n");
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
