#include "core/profile/CodeIdentity.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace crome {
namespace {
using Bytes = std::vector<unsigned char>;

std::uint16_t u16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(p[1] << 8);
}
std::uint32_t u32(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::string fnvHex(const Bytes& bytes) {
    std::uint64_t value = 0xCBF29CE484222325ULL;
    for (auto b : bytes) { value ^= b; value *= 0x100000001B3ULL; }
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setfill('0')
        << std::setw(16) << value;
    return out.str();
}
std::string normalizeSha(std::string hash) {
    if (hash.size() >= 2 && hash.substr(0, 2) == "0x") hash.erase(0, 2);
    std::transform(hash.begin(), hash.end(), hash.begin(), [](unsigned char c){
        return static_cast<char>(std::tolower(c));
    });
    return hash;
}
bool fnvMatches(const std::string& expected, const std::string& actual) {
    try {
        std::size_t posA = 0, posB = 0;
        const auto a = std::stoull(expected, &posA, 0);
        const auto b = std::stoull(actual, &posB, 0);
        return posA == expected.size() && posB == actual.size() && a == b;
    } catch (...) { return false; }
}
std::string sha256(const Bytes& bytes) {
#ifdef _WIN32
    if (bytes.size() > std::numeric_limits<ULONG>::max())
        throw std::runtime_error("secao .text grande demais");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("BCryptOpenAlgorithmProvider falhou");
    ULONG hashLength = 0, written = 0;
    auto result = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                   reinterpret_cast<PUCHAR>(&hashLength),
                                   sizeof(hashLength), &written, 0);
    if (result < 0 || hashLength != 32) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("BCryptGetProperty SHA256 falhou");
    }
    Bytes digest(hashLength);
    result = BCryptHash(algorithm, nullptr, 0,
                        const_cast<PUCHAR>(bytes.data()),
                        static_cast<ULONG>(bytes.size()), digest.data(),
                        static_cast<ULONG>(digest.size()), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) throw std::runtime_error("BCryptHash falhou");
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto b : digest) out << std::setw(2) << static_cast<unsigned>(b);
    return out.str();
#else
    (void)bytes;
    throw std::runtime_error("CodeIdentity SHA256 requer Windows");
#endif
}
struct Fingerprint {
    std::uint32_t timestamp{}, imageSize{}, textRva{}, textSize{};
    std::string fnv, sha;
};
Fingerprint readFingerprint(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("nao foi possivel abrir o EXE do processo");
    const auto end = file.tellg();
    if (end < std::streamoff(512)) throw std::runtime_error("arquivo PE pequeno demais");
    const auto fileSize = static_cast<std::uint64_t>(end);
    auto readAt = [&](std::uint64_t offset, std::size_t count) -> Bytes {
        if (offset > fileSize || count > fileSize - offset)
            throw std::runtime_error("secao/cabecalho PE fora do arquivo");
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        Bytes result(count);
        file.read(reinterpret_cast<char*>(result.data()),
                  static_cast<std::streamsize>(count));
        if (!file || static_cast<std::size_t>(file.gcount()) != count)
            throw std::runtime_error("falha na leitura do PE");
        return result;
    };
    const auto dos = readAt(0, 64);
    if (dos[0] != 'M' || dos[1] != 'Z') throw std::runtime_error("MZ invalido");
    const auto peOffset = std::uint64_t(u32(dos.data() + 0x3C));
    const auto pe = readAt(peOffset, 24);
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0)
        throw std::runtime_error("assinatura PE invalida");
    if (u16(pe.data() + 4) != 0x8664) throw std::runtime_error("arquitetura nao e x64");
    const auto sections = u16(pe.data() + 6);
    const auto optionalSize = u16(pe.data() + 20);
    if (sections == 0 || sections > 96 || optionalSize < 60)
        throw std::runtime_error("layout do PE invalido");
    const auto optional = readAt(peOffset + 24, optionalSize);
    if (u16(optional.data()) != 0x20B) throw std::runtime_error("PE32+ nao encontrado");
    const auto table = readAt(peOffset + 24 + optionalSize,
                              static_cast<std::size_t>(sections) * 40);
    Fingerprint result;
    result.timestamp = u32(pe.data() + 8);
    result.imageSize = u32(optional.data() + 56);
    for (std::uint16_t i = 0; i < sections; ++i) {
        const auto* s = table.data() + static_cast<std::size_t>(i) * 40;
        if (!(s[0] == '.' && s[1] == 't' && s[2] == 'e' &&
              s[3] == 'x' && s[4] == 't' && s[5] == 0)) continue;
        result.textRva = u32(s + 12);
        result.textSize = u32(s + 16);
        const auto rawOffset = u32(s + 20);
        if (result.textSize == 0 || result.textSize > 256U * 1024U * 1024U)
            throw std::runtime_error("tamanho da secao .text invalido");
        const auto code = readAt(rawOffset, result.textSize);
        result.fnv = fnvHex(code);
        result.sha = sha256(code);
        return result;
    }
    throw std::runtime_error("secao .text ausente");
}
} // namespace

CodeIdentityReport CodeIdentity::checkFile(const std::filesystem::path& path,
                                            const BuildInfo& expected) {
    CodeIdentityReport report;
    try {
        if (expected.textHash.empty() && expected.textHashSha256.empty()) {
            report.reason = "perfil sem hash de codigo: validacao bloqueada";
            return report;
        }
        const auto actual = readFingerprint(path);
        report.textSha256 = actual.sha;
        report.textFnv1a64 = actual.fnv;
        if (expected.timestamp && expected.timestamp != actual.timestamp) {
            report.reason = "timestamp PE diferente"; return report;
        }
        if (expected.sizeOfImage && expected.sizeOfImage != actual.imageSize) {
            report.reason = "SizeOfImage diferente"; return report;
        }
        if (expected.textRva && expected.textRva != actual.textRva) {
            report.reason = "RVA da .text diferente"; return report;
        }
        if (expected.textSize && expected.textSize != actual.textSize) {
            report.reason = "tamanho raw da .text diferente"; return report;
        }
        if (!expected.textHashSha256.empty() &&
            normalizeSha(expected.textHashSha256) != normalizeSha(actual.sha)) {
            report.reason = "SHA256 da .text diferente"; return report;
        }
        if (!expected.textHash.empty() &&
            !fnvMatches(expected.textHash, actual.fnv)) {
            report.reason = "FNV1A64 da .text diferente"; return report;
        }
        report.compatible = true;
        report.reason = "identidade do codigo compativel (hash completo ignorado)";
        return report;
    } catch (const std::exception& e) {
        report.reason = std::string("nao foi possivel validar o PE: ") + e.what();
        return report;
    }
}
} // namespace crome
