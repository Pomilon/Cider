// dedup.cpp — exact-byte deduplication of read-only blobs.
//
// Lowering gives every string and wide literal its own label (`s_N` / `w_N`),
// so a program that writes "error" ten times carries ten identical copies.
// This pass keeps the first blob for each exact byte string, rewrites every
// reference to the later copies, and removes the duplicates. The first label
// stays canonical, and the surviving order is the lowering order, so the
// mapping depends only on module contents and is deterministic.
//
// Two kinds of blob are pinned. Assembly is opaque to the IR: a label named
// by module-level or inline assembly keeps its definition even when its bytes
// match an earlier blob, and no reference is rewritten away from it. Codegen
// resolves an asm placeholder through the DataAddr result that feeds it, so
// rewriting that instruction's label is safe; the pin covers literal
// spellings. A blob lowered from a literal converted to a byte pointer is
// pinned too: CIDER-BUG-056 promises each such occurrence a distinct address,
// which pooling would erase, so the address-identity flag travels on the
// Rodata entry from lowering. A label that collides with a function or global
// name, or that is itself duplicated, is pinned for the same conservative
// reason: removing it could change a reference that names the other
// definition.
//
// Rodata carries no alignment field and codegen emits blobs back to back, so
// merging exact byte strings cannot change an alignment requirement. No
// exported symbol is renamed: rodata labels are object-local, and globals keep
// their own labels and visibility.

#include "opt/opt.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cider {
namespace opt {
namespace {

bool isSymbolChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Visits each maximal symbol-shaped word in `text`. This is the same
// boundary-aware spelling dce.cpp uses: `s_1` does not mention `s_10`, and
// `prefix_s_1` does not mention `s_1`.
template <typename Visit>
void forEachSymbolWord(const std::string& text, Visit visit) {
    std::uint64_t at = 0;
    while (at < text.size()) {
        if (!isSymbolChar(text[at])) {
            ++at;
            continue;
        }
        const std::uint64_t start = at;
        while (at < text.size() && isSymbolChar(text[at])) {
            ++at;
        }
        visit(text.substr(static_cast<std::size_t>(start),
                          static_cast<std::size_t>(at - start)));
    }
}

void collectAsmWords(const std::string& text,
                     std::unordered_set<std::string>& words) {
    forEachSymbolWord(text, [&](const std::string& word) { words.insert(word); });
}

}  // namespace

RodataDedupResult deduplicateRodata(ir::Module& module) {
    RodataDedupResult result;
    if (module.rodata.empty()) {
        return result;
    }

    // Every symbol word in assembly pins the rodata label it spells. Inline
    // assembly in a dead function is scanned too: before whole-program
    // pruning the definition is still part of the object, and a wrong guess
    // would leave an undefined reference in the emitted assembly.
    std::unordered_set<std::string> asmWords;
    collectAsmWords(module.moduleAsm, asmWords);
    for (const ir::Function& fn : module.functions) {
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                if (instr.op != ir::Op::Asm) {
                    continue;
                }
                collectAsmWords(instr.asmText, asmWords);
                for (const std::string& name : instr.asmNames) {
                    collectAsmWords(name, asmWords);
                }
            }
        }
    }

    // A rodata label that shares a spelling with another definition cannot be
    // removed safely: a reference to that spelling might name the other
    // definition, and the IR does not carry enough information to tell.
    std::unordered_set<std::string> otherNames;
    for (const ir::Function& fn : module.functions) {
        otherNames.insert(fn.name);
    }
    for (const ir::Global& global : module.globals) {
        otherNames.insert(global.label);
    }
    std::unordered_map<std::string, std::uint64_t> rodataNameCounts;
    for (const ir::Rodata& blob : module.rodata) {
        ++rodataNameCounts[blob.label];
    }

    std::unordered_set<std::string> pinned;
    for (const ir::Rodata& blob : module.rodata) {
        if (asmWords.find(blob.label) != asmWords.end()) {
            pinned.insert(blob.label);
            ++result.asmPinned;
            continue;
        }
        if (blob.addressIdentity) {
            // Lowering saw a literal-to-byte-pointer conversion. CIDER-BUG-056
            // makes each such occurrence a distinct object, so its address
            // must not become another blob's address.
            pinned.insert(blob.label);
            ++result.identityPinned;
            continue;
        }
        if (rodataNameCounts[blob.label] > 1 ||
            otherNames.find(blob.label) != otherNames.end()) {
            pinned.insert(blob.label);
        }
    }

    std::unordered_map<std::string, std::string> canonical;
    std::unordered_map<std::string, std::string> rewrite;
    std::vector<ir::Rodata> kept;
    kept.reserve(module.rodata.size());
    for (ir::Rodata& blob : module.rodata) {
        if (pinned.find(blob.label) != pinned.end()) {
            kept.push_back(std::move(blob));
            continue;
        }
        auto found = canonical.find(blob.bytes);
        if (found == canonical.end()) {
            canonical.emplace(blob.bytes, blob.label);
            kept.push_back(std::move(blob));
            continue;
        }
        rewrite.emplace(blob.label, found->second);
        ++result.blobsMerged;
        result.bytesRemoved += blob.bytes.size();
    }
    module.rodata = std::move(kept);

    auto rewriteOne = [&](std::string& label) {
        auto found = rewrite.find(label);
        if (found == rewrite.end()) {
            return;
        }
        label = found->second;
        ++result.referencesRewritten;
    };

    for (ir::Function& fn : module.functions) {
        for (ir::Block& block : fn.blocks) {
            for (ir::Instr& instr : block.body) {
                rewriteOne(instr.globalLabel);
            }
        }
    }
    for (ir::Global& global : module.globals) {
        rewriteOne(global.initLabel);
        for (std::string& label : global.initLabels) {
            rewriteOne(label);
        }
        for (auto& reloc : global.relocWords) {
            rewriteOne(reloc.second);
        }
    }
    return result;
}

}  // namespace opt
}  // namespace cider
