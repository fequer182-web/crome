#include "core/profile/ProfileRegistry.hpp"
#include "core/profile/ProfileLoader.hpp"
namespace crome {
void ProfileRegistry::loadDirectory(const std::filesystem::path& dir){
    profiles_.clear();
    if(!std::filesystem::exists(dir)) return;
    for(const auto& e: std::filesystem::directory_iterator(dir)){
        if(!e.is_regular_file() || e.path().extension()!=".json") continue;
        try { profiles_.push_back(ProfileLoader::load(e.path())); } catch(...) {}
    }
}
const BuildProfile* ProfileRegistry::find(const BuildFingerprint& fp) const{
    for(const auto& p:profiles_){
        if (p.build.timestamp != fp.timestamp || p.build.sizeOfImage != fp.sizeOfImage)
            continue;
        if (p.build.textRva && fp.textRva && p.build.textRva != fp.textRva)
            continue;
        if (p.build.textSize && fp.textSize && p.build.textSize != fp.textSize)
            continue;
        // Prefer SHA256 when the profile requires it. Legacy profiles use FNV1A64.
        if (!p.build.textHashSha256.empty()) {
            if (fp.textSha256.empty() || p.build.textHashSha256 != fp.textSha256)
                continue;
        } else if (p.build.textHash.empty() || fp.textHash.empty() ||
                   p.build.textHash != fp.textHash) {
            continue;
        }
        return &p;
    }
    return nullptr;
}
}
