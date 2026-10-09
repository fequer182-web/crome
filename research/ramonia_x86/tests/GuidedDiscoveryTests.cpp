#include "../src/RamoniaResearchProbe.cpp"
#include "../../../Crome/Developer/native_runtime/profiles/MiniJson.hpp"
#include <stdexcept>

void check(bool condition,const char* message) {
    if(!condition) throw std::runtime_error(message);
}

int main() {
    try {
        guided_discovery::AsciiHints hints("light");
        const std::string a="WorldLi",b="ght\0other",c="LightGap";
        hints.feed(reinterpret_cast<const uint8_t*>(a.data()),a.size());
        const std::array<uint8_t,6> tail{'g','h','t',0,'x',0};
        hints.feed(tail.data(),tail.size());
        check(hints.result()==std::vector<std::string>{"WorldLight"},"chunk boundary lost string");
        hints.feed(reinterpret_cast<const uint8_t*>(c.data()),c.size());
        hints.gap(); hints.finish();
        check(hints.result().size()==1,"unreadable gap fabricated match");

        Report r;
        r.module.timestamp=0x12345678;
        r.module.sizeOfImage=0x10000;
        r.fp.ok=true; r.fp.textHash=0xDEADBEAF;
        auto& bind=r.binds["move"];
        bind.selectedOk=true; bind.selected.methodRva=0x2000;
        bind.selected.ownerRva=0x6000;
        BindCandidate other; other.methodRva=0x3000; other.ownerRva=0x6000;
        bind.candidates={bind.selected,other,other};
        check(guided_discovery::candidates(bind)==std::vector<uint32_t>({0x2000,0x3000}),"alternative lost or duplicated");
        const auto signature=GuidedCandidateSignature(bind);
        bind.candidates[1].score=999;
        check(signature==GuidedCandidateSignature(bind),"score change invalidated identical addresses");
        bind.candidates[1].ownerRva++;
        check(signature!=GuidedCandidateSignature(bind),"owner change retained evidence");
        bind.candidates[1].ownerRva--;

        V33Capture capture{};
        capture.methodRva=0x2000; capture.threadId=42;
        capture.ecx=0xDEADC000; // Historical pointer must never be dereferenced.
        capture.argSnap[0].address=0xDEADB000;
        capture.argSnap[0].valid=48;
        for(size_t i=0;i<48;++i) capture.argSnap[0].bytes[i]=static_cast<uint8_t>(i);
        const auto frozen=GuidedCaptureJson(r,capture);
        auto value=crome::json::Parser(frozen).parse();
        check(value.get("savedPointersAreHistorical")->boolean(),"live pointer claim");
        check(value.get("arguments")->array()[0].get("direct")->get("bytesHex")->string().size()==96,"snapshot bytes truncated");
        std::vector<std::string> snapshots{frozen,"{\"methodRva\":\"0x3000\",\"status\":\"no_hit\"}"};
        const auto evidence=GuidedMethodJson(signature,snapshots,"entry_observations_pending_contract");
        value=crome::json::Parser(evidence).parse();
        check(!value.get("callable")->boolean() && !value.get("contractComplete")->boolean(),"entry enabled ABI");
        check(GuidedSavedObservations(evidence)==snapshots,"nested snapshots lost on resume");
        check(GuidedSavedObservations("{\"observations\":[{\"nested\":{}").empty(),"truncated JSON accepted");
        r.guidedEvidence["move"]=evidence;
        const auto saved=SaveJson(r);
        check(!saved.empty(),"profile save failed");
        value=crome::json::Parser(ReadWholeFile(saved)).parse();
        check(value.get("semanticBinds")->get("move")->get("candidates")->array().size()==3,"saved candidates lost");
        const auto resumed=ParseProfileFile(saved,r);
        check(resumed.exactBuild && resumed.guidedEvidence.at("move")==evidence,"resume lost guided observations");
        r.fp.textHash++;
        check(!ParseProfileFile(saved,r).exactBuild,"different build inherited captures");
        r.fp.textHash--;
        // Declining the guide preserves an identical current-build observation.
        std::istringstream answers("n\nn\n");
        auto* previous=std::cin.rdbuf(answers.rdbuf());
        RunGuidedDiscoveryValidation(r);
        check(r.guidedEvidence.at("move")==evidence,"skip discarded observations");
        bind.selected.methodRva++;
        RunGuidedDiscoveryValidation(r);
        std::cin.rdbuf(previous);
        check(!r.guidedEvidence.count("move"),"changed address retained old observations");
        check(r.localPlayerPtr==0 && r.actionAbi.empty(),"guide requires LP or promotes legacy ABI");
        // Walk the complete missing-method sequence without a LocalPlayer or
        // breakpoints: offer help and preserve explicit discovery prerequisites.
        r.binds.clear(); r.guidedEvidence.clear();
        std::string input="s\n\n";
        for(size_t i=0;i<guided_discovery::methods.size();++i) input+="n\n";
        std::istringstream missingAnswers(input);
        previous=std::cin.rdbuf(missingAnswers.rdbuf());
        RunGuidedDiscoveryValidation(r);
        std::cin.rdbuf(previous);
        check(r.guidedEvidence.size()==guided_discovery::methods.size(),"missing methods disappeared from guide");
        for(const auto& entry:r.guidedEvidence)
            check(JsonStringValue(entry.second,"status")=="discovery_required","missing method promoted");
        std::cout << "Guided discovery: chunk boundaries, candidates, frozen evidence, exact resume and no promotion PASS\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
