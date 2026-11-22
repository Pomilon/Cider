// sroa.cpp — promote the fields of a non-escaping aggregate Alloca to SSA.
//
// Runs after mem2reg, which has already promoted the lowerer's single-word
// scalar slots. What remains are aggregates whose words are addressed by
// constant displacements. When every access to such a slot is a direct,
// non-volatile, non-trapping Load/Store in the slot's own block, and every
// Load has a reaching Store of the same width and type, each distinct
// displacement is one scalar field: replace the Loads with the reaching values
// and delete the Alloca and all its accesses. Anything less certain — a GEP,
// an escape, asm, a link label, a width/type mismatch, a Load with no reaching
// Store — leaves the slot in memory untouched.

#include "opt/opt.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cider {
namespace opt {
namespace {

constexpr uint64_t kOneResult = 1;

bool namesBase(ir::Op op) {
    return op == ir::Op::Load || op == ir::Op::Store ||
           op == ir::Op::Extract || op == ir::Op::Insert ||
           op == ir::Op::Gep;
}

// Bytes touched by a value of this width, mirroring memory.cpp's rule: a
// Store whose width is unset writes a whole eight-byte cell.
uint8_t widthBytes(ir::IRType width) {
    switch (width) {
        case ir::IRType::I8:
        case ir::IRType::U8:
        case ir::IRType::Bool:
            return 1;
        case ir::IRType::I16:
        case ir::IRType::U16:
        case ir::IRType::I32:
        case ir::IRType::U32:
        case ir::IRType::F32:
            return 4;
        case ir::IRType::I64:
        case ir::IRType::U64:
        case ir::IRType::F64:
        case ir::IRType::Ptr:
        case ir::IRType::Void:
            return 8;
    }
    return 0;
}

// The declared extent of a slot, used only to bound a field's byte width.
// Lowering does not always stamp an Alloca's pointee, so an unknown or narrow
// declaration falls back to the one-cell width.
int64_t slotExtent(const ir::Instr& alloca) {
    int64_t extent = 0;
    if (!alloca.results.empty() && !alloca.results[0].type.isError() &&
        alloca.results[0].type.type != nullptr) {
        const Type* pointee = alloca.results[0].type.type->pointee();
        if (pointee != nullptr && !pointee->isError()) {
            extent = typeSize(pointee);
        }
    }
    if (extent < static_cast<int64_t>(layout::kSizePointer)) {
        extent = static_cast<int64_t>(layout::kSizePointer);
    }
    return extent;
}

struct FieldShape {
    ir::IRType width = ir::IRType::Void;
    TypeRef type;

    bool operator==(const FieldShape& other) const {
        return width == other.width && type == other.type;
    }
};

// Rewrite every semantic use of a promoted Load's result. Definitions are not
// touched: the map only names results of instructions this pass deletes.
void replaceUses(ir::Function& fn,
                 const std::unordered_map<uint32_t, ir::Value>& subst) {
    if (subst.empty()) {
        return;
    }
    auto remap = [&](ir::Value& value) {
        auto found = subst.find(value.id);
        if (found != subst.end()) {
            value = found->second;
        }
    };
    for (ir::Block& block : fn.blocks) {
        for (ir::Instr& instr : block.body) {
            for (ir::Value& operand : instr.operands) {
                remap(operand);
            }
            for (ir::Argument& arg : instr.arguments) {
                for (ir::Value& word : arg.words) {
                    remap(word);
                }
            }
            for (ir::PhiIncoming& incoming : instr.phiIncoming) {
                remap(incoming.value);
            }
            if (namesBase(instr.op) && instr.globalLabel.empty()) {
                remap(instr.addressBase);
            }
            if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                remap(instr.storedValue);
            }
            if (instr.op == ir::Op::Call && instr.call.indirect) {
                remap(instr.calleeValue);
            }
        }
        ir::Terminator& term = block.term;
        for (ir::Value& word : term.values) {
            remap(word);
        }
        if (term.cond.has_value()) {
            remap(*term.cond);
        }
        if (term.term == ir::Term::Switch) {
            remap(term.switchValue);
            for (ir::Case& arm : term.cases) {
                remap(arm.value);
            }
        }
    }
}

bool isParam(const ir::Function& fn, uint32_t id) {
    for (const ir::Value& param : fn.params) {
        if (param.id == id) {
            return true;
        }
    }
    return false;
}

}  // namespace

SroaResult sroa(ir::Module& module) {
    SroaResult result;
    for (ir::Function& fn : module.functions) {
        const SlotFacts facts = analyzeMemory(fn);
        if (facts.asmPresent || facts.nonEscaping.empty()) {
            continue;
        }

        // A constant GEP still escapes the slot from SROA's point of view: its
        // result is a pointer this pass cannot track precisely.
        std::unordered_set<uint32_t> gepBases;
        for (const auto& entry : facts.gepDisplacement) {
            gepBases.insert(entry.first);
        }

        // Read-only eligibility pass. A slot qualifies only when all of its
        // accesses are direct field accesses in its own block after the
        // Alloca, every field's accesses agree on width and type, and every
        // Load is preceded in program order by a Store to that field.
        std::unordered_set<uint32_t> eligible;
        for (uint32_t slot : facts.nonEscaping) {
            if (gepBases.find(slot) != gepBases.end() || isParam(fn, slot)) {
                continue;
            }
            uint64_t allocaBlock = 0;
            uint64_t allocaIndex = 0;
            int64_t extent = 0;
            bool foundAlloca = false;
            for (uint64_t bi = 0; bi < fn.blocks.size(); ++bi) {
                for (uint64_t ii = 0; ii < fn.blocks[bi].body.size(); ++ii) {
                    const ir::Instr& candidate = fn.blocks[bi].body[ii];
                    if (candidate.op != ir::Op::Alloca) {
                        continue;
                    }
                    for (const ir::Value& value : candidate.results) {
                        if (value.id == slot) {
                            foundAlloca = true;
                            allocaBlock = bi;
                            allocaIndex = ii;
                            extent = slotExtent(candidate);
                        }
                    }
                }
            }
            if (!foundAlloca) {
                continue;
            }

            bool ok = true;
            std::unordered_map<int64_t, FieldShape> fieldShape;
            std::unordered_set<int64_t> hasStore;
            for (uint64_t bi = 0; bi < fn.blocks.size() && ok; ++bi) {
                for (uint64_t ii = 0; ii < fn.blocks[bi].body.size(); ++ii) {
                    const ir::Instr& instr = fn.blocks[bi].body[ii];
                    if (instr.op == ir::Op::Alloca) {
                        continue;
                    }
                    if (!namesBase(instr.op) || instr.addressBase.id != slot) {
                        continue;
                    }
                    // Extract, Insert and Gep over the slot are not fields.
                    if (instr.op != ir::Op::Load && instr.op != ir::Op::Store) {
                        ok = false;
                        break;
                    }
                    // Volatile, trapping, link-labelled and cross-block
                    // accesses are all barriers for this pass.
                    if (!instr.globalLabel.empty() || instr.isVolatile ||
                        instr.mayTrap || bi != allocaBlock || ii <= allocaIndex ||
                        instr.displacement < 0) {
                        ok = false;
                        break;
                    }
                    const uint8_t bytes = widthBytes(instr.width);
                    if (bytes == 0 || bytes > extent) {
                        ok = false;
                        break;
                    }
                    const FieldShape shape{instr.width, instr.type};
                    if (instr.op == ir::Op::Store) {
                        const ir::Value& value = instr.storedValue;
                        const uint8_t valueBytes = widthBytes(value.width);
                        if (value.width == ir::IRType::Void || value.type.isError() ||
                            valueBytes == 0 || valueBytes > bytes) {
                            ok = false;
                            break;
                        }
                        auto known = fieldShape.find(instr.displacement);
                        if (known != fieldShape.end() && !(known->second == shape)) {
                            ok = false;
                            break;
                        }
                        fieldShape[instr.displacement] = shape;
                        hasStore.insert(instr.displacement);
                    } else {
                        if (instr.results.size() != kOneResult ||
                            hasStore.find(instr.displacement) == hasStore.end()) {
                            ok = false;
                            break;
                        }
                        auto known = fieldShape.find(instr.displacement);
                        if (known == fieldShape.end() || !(known->second == shape)) {
                            ok = false;
                            break;
                        }
                    }
                }
            }
            if (ok && !fieldShape.empty()) {
                eligible.insert(slot);
            }
        }
        if (eligible.empty()) {
            continue;
        }

        // Apply pass. Process instructions in program order so a Store to a
        // field replaces the value a later Load of that field receives, and
        // canonicalise a stored value that is itself a promoted Load result.
        const uint64_t blockCount = fn.blocks.size();
        std::vector<std::vector<uint8_t>> drop(blockCount);
        const uint8_t keepFlag = 0;
        for (uint64_t bi = 0; bi < blockCount; ++bi) {
            drop[bi].assign(fn.blocks[bi].body.size(), keepFlag);
        }
        std::unordered_map<uint32_t, ir::Value> subst;
        std::unordered_map<uint32_t, std::unordered_map<int64_t, ir::Value>>
            fieldValues;
        std::unordered_set<uint32_t> scalarized;

        for (uint64_t bi = 0; bi < blockCount; ++bi) {
            for (uint64_t ii = 0; ii < fn.blocks[bi].body.size(); ++ii) {
                ir::Instr& instr = fn.blocks[bi].body[ii];
                if (instr.op == ir::Op::Alloca) {
                    for (const ir::Value& value : instr.results) {
                        if (eligible.find(value.id) != eligible.end()) {
                            drop[bi][ii] = 1;
                            scalarized.insert(value.id);
                        }
                    }
                    continue;
                }
                if (!namesBase(instr.op)) {
                    continue;
                }
                const uint32_t slot = instr.addressBase.id;
                if (eligible.find(slot) == eligible.end() ||
                    !instr.globalLabel.empty() || instr.isVolatile ||
                    instr.mayTrap) {
                    continue;
                }
                if (instr.op == ir::Op::Store) {
                    ir::Value value = instr.storedValue;
                    auto mapped = subst.find(value.id);
                    if (mapped != subst.end()) {
                        value = mapped->second;
                    }
                    fieldValues[slot][instr.displacement] = value;
                    drop[bi][ii] = 1;
                    ++result.storesRemoved;
                } else if (instr.op == ir::Op::Load) {
                    auto fields = fieldValues.find(slot);
                    if (fields == fieldValues.end()) {
                        continue;
                    }
                    auto value = fields->second.find(instr.displacement);
                    if (value == fields->second.end()) {
                        continue;
                    }
                    subst[instr.results[0].id] = value->second;
                    drop[bi][ii] = 1;
                    ++result.loadsRemoved;
                }
            }
        }

        replaceUses(fn, subst);
        for (uint64_t bi = 0; bi < blockCount; ++bi) {
            std::vector<ir::Instr> kept;
            for (uint64_t ii = 0; ii < fn.blocks[bi].body.size(); ++ii) {
                if (drop[bi][ii] == 0) {
                    kept.push_back(std::move(fn.blocks[bi].body[ii]));
                }
            }
            fn.blocks[bi].body = std::move(kept);
        }
        result.slotsScalarized += scalarized.size();
        for (uint32_t slot : scalarized) {
            auto fields = fieldValues.find(slot);
            if (fields != fieldValues.end()) {
                result.fieldsPromoted += fields->second.size();
            }
        }
    }
    return result;
}

}  // namespace opt
}  // namespace cider
