#pragma once
#include "core/profile/BuildProfile.hpp"
#include <filesystem>
#include <optional>
#include <vector>
namespace crome {
struct BuildFingerprint { uint64_t timestamp{}, sizeOfImage{}; std::string textHash, textSha256; uint64_t textRva{}, textSize{}; };
class ProfileRegistry {
    std::vector<BuildProfile> profiles_;
public:
    void loadDirectory(const std::filesystem::path& dir);
    const std::vector<BuildProfile>& all() const { return profiles_; }
    const BuildProfile* find(const BuildFingerprint& fp) const;
};
}
