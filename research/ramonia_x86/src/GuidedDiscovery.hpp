#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace guided_discovery {
inline constexpr std::array<const char*,26> methods{
    "look","talkChannel","talk","move","use","useWith","attack","follow","autoWalk",
    "getTile","getCreatureById","getPosition","getThings","getThingCount",
    "getTopMoveThing","getTopUseThing","isWalkable","isPathable","getInventoryItem",
    "getId","getCount","getContainers","getItems","getContainer","getName","getCapacity"
};

// Keep every distinct current-build address. A score is only an ordering hint.
template<class Bind> std::vector<uint32_t> candidates(const Bind& bind) {
    std::vector<uint32_t> result;
    if(bind.selectedOk && bind.selected.methodRva) result.push_back(bind.selected.methodRva);
    for(const auto& candidate:bind.candidates)
        if(candidate.methodRva && std::find(result.begin(),result.end(),candidate.methodRva)==result.end())
            result.push_back(candidate.methodRva);
    return result;
}

inline const char* instruction(const std::string& method) {
    if(method=="look" || method=="getId" || method=="getName")
        return "De LOOK em um item ou criatura. Na segunda amostra, use OUTRO objeto.";
    if(method=="talk" || method=="talkChannel")
        return "Fale ABI_TEST_ALPHA no Default; na segunda amostra, fale ABI_TEST_BETA.";
    if(method=="move" || method=="getTopMoveThing" || method=="getThings" || method=="getThingCount")
        return "Arraste um item do CHAO normalmente. Na segunda amostra, use OUTRO item/tile.";
    if(method=="use" || method=="getTopUseThing")
        return "Use um objeto do CHAO normalmente. Na segunda amostra, use OUTRO objeto/tile.";
    if(method=="useWith") return "Use uma ferramenta/runa COM um alvo; repita com OUTRO alvo na segunda amostra.";
    if(method=="attack") return "Ataque uma criatura e mantenha o target; na segunda amostra, ataque OUTRA.";
    if(method=="follow") return "Use FOLLOW em uma criatura; na segunda amostra, siga OUTRA.";
    if(method=="getContainer" || method=="getContainers" || method=="getItems")
        return "Abra um backpack/container; na segunda amostra, abra OUTRO container.";
    if(method=="getInventoryItem" || method=="getCount")
        return "Mova/equipe um item no inventario; na segunda amostra, use OUTRO item/quantidade.";
    return "Ande/clique no mapa pelo cliente; na segunda amostra, use OUTRO destino.";
}

// Streaming ASCII hints preserve strings across chunks but never across an
// unreadable gap or section boundary. No per-byte system calls are needed.
struct AsciiHints {
    std::string needle, current;
    std::set<std::string> matches;
    explicit AsciiHints(std::string wanted):needle(std::move(wanted)) {
        for(char& c:needle) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    void finish() {
        auto lower=current;
        for(char& c:lower) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if(current.size()>=4 && lower.find(needle)!=std::string::npos) matches.insert(current);
        current.clear();
    }
    void gap() { current.clear(); }
    void feed(const uint8_t* bytes,size_t size) {
        for(size_t i=0;i<size;++i) {
            const auto c=bytes[i];
            if(c>=0x20 && c<=0x7E) { if(current.size()<256) current.push_back(static_cast<char>(c)); }
            else finish();
        }
    }
    std::vector<std::string> result() const {
        std::vector<std::string> out(matches.begin(),matches.end());
        if(out.size()>64) out.resize(64);
        return out;
    }
};
}
