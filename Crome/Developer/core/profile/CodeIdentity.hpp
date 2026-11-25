#pragma once
#include "core/profile/BuildProfile.hpp"
#include <filesystem>
#include <string>

namespace crome {

// Identidade do codigo carregado a partir do arquivo PE no disco.
// Nao usa SHA-256 do arquivo inteiro para bloquear instalacoes.
struct CodeIdentityReport {
    bool compatible{};
    std::string reason;
    std::string textSha256;
    std::string textFnv1a64;
};
class CodeIdentity {
public:
    static CodeIdentityReport checkFile(const std::filesystem::path& executable,
                                        const BuildInfo& expected);
};
} // namespace crome
