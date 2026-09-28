#pragma once

#include <set>
#include <string>

namespace cider {

// CIDER-BUG-072: main-file symbols keep their bare Cider names, but NASM
// resolves some spellings before it will accept them as a label, so a
// global or function named `gs`, `ss`, `bp`, `byte`, `align`, ... used to
// emit invalid assembly:
//
//     var gs: int = 7;  ->  gs: dd 7            (segment register: error)
//                           mov eax, [rel gs]   (invalid effective address)
//
// The set below covers the token classes NASM refuses in a label
// definition or in a `[rel NAME]` / `call NAME` / `dq NAME` reference —
// the contexts this emitter produces: registers, instruction prefixes,
// operand-size/addressing keywords, pseudo-instructions and directives,
// and the preprocessor specials. Instruction mnemonics are deliberately
// absent: NASM accepts `mov:` / `add:` as labels (the colon
// disambiguates), so escaping them would change long-working output for
// no benefit.
inline const std::set<std::string>& nasmReservedWords() {
    static const std::set<std::string> kWords = [] {
        std::set<std::string> words = {
            // Instruction prefixes (NASM manual §3.4.1).
            "bnd", "lock", "rep", "repe", "repne", "repnz", "repz",
            "xacquire", "xrelease",
            // Operand-size, addressing and distance keywords.
            "a16", "a32", "a64", "o16", "o32", "o64", "abs", "byte",
            "dword", "far", "near", "nosplit", "oword", "qword", "rel",
            "seg", "short", "strict", "tword", "word", "wrt", "yword",
            "zword",
            // Pseudo-instructions, directives and preprocessor specials.
            "absolute", "align", "alignb", "bits", "common", "cpu", "db",
            "dd", "default", "do", "dq", "dt", "dw", "dy", "dz",
            "endmacro", "equ", "error", "extern", "fatal", "float",
            "global", "group", "incbin", "include", "inline", "macro",
            "org", "postfix", "prefix", "resb", "resd", "reso", "resq",
            "rest", "resw", "resy", "resz", "sectalign", "section",
            "segment", "static", "times", "use16", "use32", "use64",
            "wait", "warning", "__BITS__", "__FILE__", "__LINE__",
        };
        // Registers: segment, general purpose (all widths), instruction
        // pointer, x87, MMX, SSE/AVX/AVX-512, mask, control/debug/test
        // and bounds.
        for (const char* r : {"cs", "ds", "es", "fs", "gs", "ss"})
            words.insert(r);
        for (const char* r : {"ax", "bx", "cx", "dx", "si", "di", "bp", "sp"}) {
            words.insert(r);
            words.insert(std::string("e") + r);
            words.insert(std::string("r") + r);
        }
        for (const char* r : {"al", "ah", "bl", "bh", "cl", "ch", "dl", "dh",
                              "sil", "dil", "spl", "bpl"})
            words.insert(r);
        for (int i = 8; i <= 15; ++i) {
            const std::string r = "r" + std::to_string(i);
            words.insert(r);
            words.insert(r + "b");
            words.insert(r + "w");
            words.insert(r + "d");
        }
        for (int i = 0; i < 8; ++i) {
            words.insert("st" + std::to_string(i));
            words.insert("mm" + std::to_string(i));
            words.insert("k" + std::to_string(i));
            words.insert("tr" + std::to_string(i));
        }
        for (int i = 0; i < 32; ++i) {
            words.insert("xmm" + std::to_string(i));
            words.insert("ymm" + std::to_string(i));
            words.insert("zmm" + std::to_string(i));
        }
        for (int i = 0; i < 16; ++i) {
            words.insert("cr" + std::to_string(i));
            words.insert("dr" + std::to_string(i));
        }
        for (int i = 0; i < 4; ++i) words.insert("bnd" + std::to_string(i));
        words.insert("rip");
        words.insert("eip");
        words.insert("ip");
        return words;
    }();
    return kWords;
}

// Deterministic escape for a colliding bare name: appending `$` keeps the
// base name readable in IR dumps and disassembly while guaranteeing the
// spelling can never alias another Cider symbol — the lexer's identifier
// alphabet is [A-Za-z0-9_], so no Cider name contains `$`. NASM accepts a
// `$` inside an identifier (only a lone `$` means the current address);
// label-definition, `[rel NAME]`, `call NAME`, `dq NAME`, `global NAME`
// and `extern NAME` forms were all verified to assemble escaped spellings.
// Callers must route every definition AND reference through this (via
// emitGlobalName / emitFunctionName), so labels and uses stay in sync.
inline std::string escapeNasmReservedWord(const std::string& name) {
    if (nasmReservedWords().count(name) != 0) return name + "$";
    return name;
}

}  // namespace cider
