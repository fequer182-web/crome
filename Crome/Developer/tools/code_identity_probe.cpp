// Verificacao isolada da identidade do codigo PE. Nunca abre o PID e nao injeta DLL.
#include "core/profile/CodeIdentity.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdint>

namespace {
std::uint64_t integerField(const nlohmann::json& obj, const char* key) {
    const auto& value = obj.at(key);
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer()) return static_cast<std::uint64_t>(value.get<std::int64_t>());
    const auto text = value.get<std::string>();
    std::size_t consumed = 0;
    const auto result = std::stoull(text, &consumed, 0);
    if (consumed != text.size()) throw std::runtime_error(std::string("numero invalido: ") + key);
    return result;
}
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Uso: code_identity_probe.exe <caminho-antica.exe> <antica_identity.json>\n";
        return 2;
    }
    try {
        std::ifstream input(argv[2]);
        if (!input) throw std::runtime_error("perfil JSON nao encontrado");
        const auto j = nlohmann::json::parse(input);
        crome::BuildInfo expected;
        expected.timestamp = integerField(j, "timestamp");
        expected.sizeOfImage = integerField(j, "sizeOfImage");
        expected.textRva = integerField(j, "textRva");
        expected.textSize = integerField(j, "textRawSize");
        expected.textHashSha256 = j.at("textSha256").get<std::string>();
        expected.textHash = j.at("textFnv1a64").get<std::string>();
        const auto r = crome::CodeIdentity::checkFile(std::filesystem::path(argv[1]), expected);
        std::cout << "Codigo compativel: " << (r.compatible ? "SIM" : "NAO") << '\n'
                  << "Motivo: " << r.reason << '\n'
                  << "SHA256 .text: " << r.textSha256 << '\n'
                  << "FNV1A64 .text: " << r.textFnv1a64 << '\n'
                  << "Runtime: NAO VALIDADO\n"
                  << "Acoes nativas: DESABILITADAS\n";
        return r.compatible ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << '\n';
        return 3;
    }
}
