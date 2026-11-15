#pragma once
// Minimal Pawn (AMX) virtual machine for the ORIGINAL Van Buren scripts (.scrb behaviour scripts, .scra dialogues).
//
// The shipped files are AMX file_version 6 / 32-bit cells / non-compact; they use exactly the standard Pawn 3.x
// opcode table (verified: all 119 scripts disassemble with zero invalid opcodes, every jump/call lands on an
// instruction boundary, every public points at PROC, every sysreq index is < #natives; see docs/server-data.md).
// compuphase's current amx.c (file version 11) cannot load them, so this is a small own interpreter
// (about 400 lines) instead of a third-party dependency.
//
// Natives are bound by NAME (the .amx native table carries names). Unbound natives return 0 and are logged once.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace f3 {

using AmxCell = int32_t;

class AmxScript;  // immutable loaded program (shared by all instances)
class AmxInstance;

using AmxNative = std::function<AmxCell(AmxInstance&, const AmxCell* args, int argc)>;

class AmxScript {
public:
    static std::shared_ptr<AmxScript> Load(const std::string& path, std::string& err);
    const std::string& Name() const { return name_; }
    bool HasPublic(const std::string& n) const { return publics_.count(n) != 0; }
    std::vector<std::string> PublicNames() const;
    const std::vector<std::string>& Natives() const { return natives_; }

private:
    friend class AmxInstance;
    std::string name_;
    std::vector<AmxCell> code_;
    std::vector<uint8_t> data_;  // initial DAT image
    int32_t hea_ = 0, stp_ = 0;
    std::map<std::string, int32_t> publics_;  // name -> code byte offset
    std::vector<std::string> natives_;
};

class AmxInstance {
public:
    AmxInstance(std::shared_ptr<AmxScript> script, const std::unordered_map<std::string, AmxNative>* natives);

    // Calls a public function. Returns false if missing or on a VM error (logged). Result in *ret.
    bool Call(const std::string& pub, const std::vector<AmxCell>& args = {}, AmxCell* ret = nullptr);
    const std::string& LastError() const { return err_; }
    const AmxScript& Script() const { return *script_; }

    // helpers for natives
    std::string GetString(AmxCell addr, size_t maxLen = 256) const;  // packed or unpacked
    AmxCell Get(AmxCell addr) const;
    void Set(AmxCell addr, AmxCell v);
    bool SetString(AmxCell addr, const std::string& s, size_t maxCells = 256);  // unpacked (one char per cell), zero terminated; false if it does not fit
    AmxCell Alloc(int cells);   // zeroed scratch cells from the heap (test harness / natives that need temporary memory); -1 if full
    static void SetTrace(bool on);  // log every native call (name + args)
    uint64_t userData = 0;  // owner entity id (critter or map object), set by the host

private:
    bool Run(int32_t cipStart);
    bool Fail(const char* why);
    AmxCell* Cell(AmxCell addr);

    std::shared_ptr<AmxScript> script_;
    const std::unordered_map<std::string, AmxNative>* natives_;
    std::vector<AmxNative const*> bound_;
    std::vector<bool> warned_;
    std::vector<uint8_t> mem_;
    AmxCell pri_ = 0, alt_ = 0, frm_ = 0, stk_ = 0, hea_ = 0;
    std::string err_;
    int steps_ = 0;
};

}  // namespace f3
