// blocks_test.cpp — empty-block elimination and single-predecessor merging.
//
// The pass runs after constant control-flow simplification, so these tests
// build the IR shapes that simplification leaves behind: empty `Br target`
// blocks, chains with one predecessor, multi-predecessor joins, and phis that
// name the block being removed. Every transform is followed by the structural
// checker, which is the same gate the pipeline runs after this pass.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "ir/ir.h"
#include "opt/opt.h"
#include "opt/structural.h"
#include "types/context.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool holds, const std::string& what) {
    ++checks;
    if (holds) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++failures;
    }
}

bool structurallyValid(const cider::ir::Module& module) {
    return cider::opt::checkModule(module).empty();
}

const cider::ir::Block* blockWithLabel(const cider::ir::Function& fn,
                                       uint32_t label) {
    for (const cider::ir::Block& block : fn.blocks) {
        if (block.label == label) {
            return &block;
        }
    }
    return nullptr;
}

const cider::ir::Instr* instructionWithResult(const cider::ir::Function& fn,
                                              uint32_t id) {
    for (const cider::ir::Block& block : fn.blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            for (const cider::ir::Value& result : instr.results) {
                if (result.id == id) {
                    return &instr;
                }
            }
        }
    }
    return nullptr;
}

bool hasIncoming(const cider::ir::Instr& phi, uint32_t block, uint32_t value) {
    for (const cider::ir::PhiIncoming& incoming : phi.phiIncoming) {
        if (incoming.block == block && incoming.value.id == value) {
            return true;
        }
    }
    return false;
}

bool mentionsBlock(const cider::ir::Instr& phi, uint32_t block) {
    for (const cider::ir::PhiIncoming& incoming : phi.phiIncoming) {
        if (incoming.block == block) {
            return true;
        }
    }
    return false;
}

std::string blockLabels(const cider::ir::Function& fn) {
    std::string labels;
    for (const cider::ir::Block& block : fn.blocks) {
        labels += std::to_string(block.label) + ",";
    }
    return labels;
}

// A stable description of block order, terminators and phi lists. Type
// pointers are never printed, so two independently built modules compare.
std::string describe(const cider::ir::Function& fn) {
    std::string out;
    for (const cider::ir::Block& block : fn.blocks) {
        out += "block " + std::to_string(block.label) + " term " +
               std::to_string(static_cast<int>(block.term.term)) + " tgt " +
               std::to_string(block.term.target) + " then " +
               std::to_string(block.term.thenTarget) + " else " +
               std::to_string(block.term.elseTarget) + " default " +
               std::to_string(block.term.defaultTarget) + "\n";
        for (const cider::ir::Instr& instr : block.body) {
            out += "  " + std::string(cider::ir::opName(instr.op));
            for (const cider::ir::Value& result : instr.results) {
                out += " r" + std::to_string(result.id);
            }
            for (const cider::ir::PhiIncoming& incoming : instr.phiIncoming) {
                out += " [" + std::to_string(incoming.value.id) + "," +
                       std::to_string(incoming.block) + "]";
            }
            out += "\n";
        }
        for (const cider::ir::Value& word : block.term.values) {
            out += "  ret " + std::to_string(word.id) + "\n";
        }
        if (block.term.cond.has_value()) {
            out += "  cond " + std::to_string(block.term.cond->id) + "\n";
        }
        if (block.term.term == cider::ir::Term::Switch) {
            out += "  switch " + std::to_string(block.term.switchValue.id) +
                   "\n";
            for (const cider::ir::Case& arm : block.term.cases) {
                out += "  case " + std::to_string(arm.value.id) + " -> " +
                       std::to_string(arm.target) + "\n";
            }
        }
    }
    return out;
}

struct Ir {
    cider::TypeContext types;
    cider::ir::Module module;
    cider::ir::Function* fn = nullptr;

    Ir() {
        module.name = "test";
        cider::ir::Function function;
        function.name = "f";
        cider::ir::Block block;
        block.label = 0;
        function.blocks.push_back(block);
        module.functions.push_back(std::move(function));
        fn = &module.functions.back();
    }

    cider::TypeRef i32() {
        return cider::TypeRef{types.basic(cider::BasicKind::I32)};
    }
    cider::TypeRef i64() {
        return cider::TypeRef{types.basic(cider::BasicKind::I64)};
    }
    cider::TypeRef boolean() {
        return cider::TypeRef{types.basic(cider::BasicKind::Bool)};
    }
    cider::TypeRef ptr() {
        return cider::TypeRef{types.pointer(types.basic(cider::BasicKind::U8))};
    }

    cider::ir::Value value(uint32_t id, cider::ir::IRType width,
                           cider::TypeRef type) {
        cider::ir::Value out;
        out.id = id;
        out.width = width;
        out.type = type;
        return out;
    }

    void param(uint32_t id, cider::ir::IRType width, cider::TypeRef type) {
        fn->params.push_back(value(id, width, type));
    }

    cider::ir::Block& addBlock(uint32_t label) {
        cider::ir::Block block;
        block.label = label;
        fn->blocks.push_back(std::move(block));
        return fn->blocks.back();
    }

    cider::ir::Block* blockAt(uint32_t label) {
        for (cider::ir::Block& block : fn->blocks) {
            if (block.label == label) {
                return &block;
            }
        }
        return nullptr;
    }

    void emit(cider::ir::Instr instr) {
        fn->blocks.back().body.push_back(std::move(instr));
    }

    cider::ir::Instr constant(uint32_t id, int64_t bits,
                              cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Const;
        instr.width = width;
        instr.type = type;
        instr.constantBits = bits;
        instr.constantText = std::to_string(bits);
        instr.isPure = true;
        instr.results.push_back(value(id, width, type));
        return instr;
    }

    cider::ir::Instr binary(cider::ir::Op op, uint32_t id,
                            cider::ir::Value lhs, cider::ir::Value rhs,
                            cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = op;
        instr.width = width;
        instr.type = type;
        instr.isPure = true;
        instr.results.push_back(value(id, width, type));
        instr.operands.push_back(lhs);
        instr.operands.push_back(rhs);
        return instr;
    }

    cider::ir::Instr phi(
        uint32_t id, cider::ir::IRType width, cider::TypeRef type,
        const std::vector<std::pair<uint32_t, uint32_t>>& incoming) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Phi;
        instr.width = width;
        instr.type = type;
        instr.isPure = true;
        instr.results.push_back(value(id, width, type));
        for (const auto& pair : incoming) {
            cider::ir::PhiIncoming entry;
            entry.value = value(pair.first, width, type);
            entry.block = pair.second;
            instr.phiIncoming.push_back(entry);
        }
        return instr;
    }

    cider::ir::Instr allocaSlot(uint32_t id, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Alloca;
        instr.width = cider::ir::IRType::Ptr;
        instr.type = cider::TypeRef{types.pointer(type.type)};
        instr.results.push_back(value(id, instr.width, instr.type));
        return instr;
    }

    cider::ir::Instr store(uint32_t base, cider::ir::Value stored,
                           cider::ir::IRType width, cider::TypeRef type,
                           bool isVolatile = false) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Store;
        instr.width = width;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr());
        instr.storedValue = stored;
        instr.isVolatile = isVolatile;
        return instr;
    }

    cider::ir::Instr asmBlock(const std::string& text = "nop") {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Asm;
        instr.width = cider::ir::IRType::Void;
        instr.asmText = text;
        return instr;
    }

    cider::ir::Instr trap() {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Trap;
        instr.width = cider::ir::IRType::Void;
        instr.reason = cider::ir::TrapReason::Explicit;
        instr.mayTrap = true;
        return instr;
    }

    void finishRet(uint32_t blockLabel, uint32_t id, cider::ir::IRType width,
                   cider::TypeRef type) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Ret;
        term.values.push_back(value(id, width, type));
        blockAt(blockLabel)->term = term;
    }

    void finishVoidRet(uint32_t blockLabel) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Ret;
        blockAt(blockLabel)->term = term;
    }

    void finishBr(uint32_t blockLabel, uint32_t target) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Br;
        term.target = target;
        blockAt(blockLabel)->term = term;
    }

    void finishCondBr(uint32_t blockLabel, cider::ir::Value cond,
                      uint32_t thenTarget, uint32_t elseTarget) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::CondBr;
        term.cond = cond;
        term.thenTarget = thenTarget;
        term.elseTarget = elseTarget;
        blockAt(blockLabel)->term = term;
    }

    void finishSwitch(uint32_t blockLabel, cider::ir::Value value,
                      const std::vector<std::pair<cider::ir::Value, uint32_t>>&
                          cases,
                      uint32_t defaultTarget) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Switch;
        term.switchValue = value;
        for (const auto& pair : cases) {
            cider::ir::Case arm;
            arm.value = pair.first;
            arm.target = pair.second;
            term.cases.push_back(arm);
        }
        term.defaultTarget = defaultTarget;
        blockAt(blockLabel)->term = term;
    }

    void finishTrap(uint32_t blockLabel) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Trap;
        term.reason = cider::ir::TrapReason::Explicit;
        blockAt(blockLabel)->term = term;
    }
};

void testEmptyBlockOnePredecessor() {
    std::printf("empty block with one predecessor is removed\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 3);
    ir.addBlock(1);
    ir.finishBr(1, 2);
    ir.addBlock(2);
    ir.emit(ir.constant(10, 42, cider::ir::IRType::I32, t));
    ir.finishRet(2, 10, cider::ir::IRType::I32, t);
    ir.addBlock(3);
    ir.emit(ir.constant(11, 7, cider::ir::IRType::I32, t));
    ir.finishBr(3, 2);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,2,3,", "the empty block is gone");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::CondBr &&
              ir.fn->blocks[0].term.thenTarget == 2 &&
              ir.fn->blocks[0].term.elseTarget == 3,
          "the predecessor edge is redirected to the target");
    check(result.emptyBlocksRemoved == 1 && result.blocksMerged == 0,
          "one empty removal is counted and no merge happens");
}

void testEmptyBlockMultiplePredecessors() {
    std::printf("empty block with multiple predecessors loses every edge\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(10, 1, cider::ir::IRType::I32, t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.constant(11, 2, cider::ir::IRType::I32, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.finishBr(3, 4);
    ir.addBlock(4);
    ir.emit(ir.constant(12, 3, cider::ir::IRType::I32, t));
    ir.finishRet(4, 12, cider::ir::IRType::I32, t);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,2,4,", "the empty join block is gone");
    check(ir.fn->blocks[1].term.term == cider::ir::Term::Br &&
              ir.fn->blocks[1].term.target == 4 &&
              ir.fn->blocks[2].term.term == cider::ir::Term::Br &&
              ir.fn->blocks[2].term.target == 4,
          "both predecessors now branch to the real target");
    check(result.emptyBlocksRemoved == 1 && result.blocksMerged == 0,
          "only the empty block is counted");
}

void testMultiplePredecessorsWithTargetPhisSkipped() {
    std::printf("a multi-predecessor empty block before phis is skipped\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.emit(ir.constant(1, 9, cider::ir::IRType::I32, t));
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(10, 1, cider::ir::IRType::I32, t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.constant(11, 2, cider::ir::IRType::I32, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.finishBr(3, 4);
    ir.addBlock(4);
    ir.emit(ir.phi(20, cider::ir::IRType::I32, t, {{1, 3}}));
    ir.finishRet(4, 20, cider::ir::IRType::I32, t);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,2,3,4,",
          "the empty block stays because the target has phis");
    check(result.emptyBlocksRemoved == 0 && result.blocksMerged == 0,
          "no counter moves");
}

void testSinglePredecessorMerge() {
    std::printf("a single-predecessor block is merged into its predecessor\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.finishBr(0, 1);
    ir.addBlock(1);
    ir.emit(ir.constant(5, 77, cider::ir::IRType::I32, t));
    ir.finishRet(1, 5, cider::ir::IRType::I32, t);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,", "the successor block is gone");
    check(ir.fn->blocks[0].body.size() == 1 &&
              ir.fn->blocks[0].body[0].op == cider::ir::Op::Const,
          "the successor body moved to the predecessor");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::Ret &&
              ir.fn->blocks[0].term.values.size() == 1 &&
              ir.fn->blocks[0].term.values[0].id == 5,
          "the successor terminator moved with it");
    check(result.blocksMerged == 1 && result.emptyBlocksRemoved == 0,
          "one merge is counted");
}

void testMultiplePredecessorsMergeSkipped() {
    std::printf("a block with several predecessors is not merged\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(10, 1, cider::ir::IRType::I32, t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.constant(11, 2, cider::ir::IRType::I32, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.constant(12, 3, cider::ir::IRType::I32, t));
    ir.finishRet(3, 12, cider::ir::IRType::I32, t);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,2,3,", "the join block stays");
    check(result.blocksMerged == 0 && result.emptyBlocksRemoved == 0,
          "no counter moves");
}

void testEntryPreserved() {
    std::printf("the entry block is never removed or merged away\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.finishBr(0, 1);
    ir.addBlock(1);
    ir.emit(ir.constant(5, 8, cider::ir::IRType::I32, t));
    ir.finishRet(1, 5, cider::ir::IRType::I32, t);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(!ir.fn->blocks.empty() && ir.fn->blocks.front().label == 0,
          "the entry label is still first");
    check(ir.fn->blocks[0].body.size() == 1 &&
              ir.fn->blocks[0].term.term == cider::ir::Term::Ret,
          "the entry absorbed its successor instead of disappearing");
    check(result.blocksMerged == 1 && result.emptyBlocksRemoved == 0,
          "the successor merge is counted, not an entry removal");
}

void testSelfLoopsSkipped() {
    std::printf("self-loops are not created or removed\n");
    {
        Ir ir;
        ir.finishBr(0, 1);
        ir.addBlock(1);
        ir.finishBr(1, 1);

        cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
        check(structurallyValid(ir.module), "the self-loop stays valid");
        check(blockLabels(*ir.fn) == "0,1,",
              "an empty block whose target is itself is not removed");
        check(result.emptyBlocksRemoved == 0 && result.blocksMerged == 0,
              "no counter moves for a self-loop candidate");
    }
    {
        Ir ir;
        ir.finishBr(0, 1);
        ir.addBlock(1);
        ir.finishBr(1, 0);

        cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
        check(structurallyValid(ir.module), "the back edge stays valid");
        check(blockLabels(*ir.fn) == "0,1,",
              "a redirect that would make the target loop is skipped");
        check(result.emptyBlocksRemoved == 0 && result.blocksMerged == 0,
              "no counter moves for a predecessor/target loop");
    }
}

void testPhiIncomingRewriteOnMerge() {
    std::printf("merge rewrites a successor phi incoming from B to P\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 4);
    ir.addBlock(1);
    ir.emit(ir.constant(20, 20, cider::ir::IRType::I32, t));
    ir.finishBr(1, 2);
    ir.addBlock(2);
    ir.emit(ir.constant(30, 30, cider::ir::IRType::I32, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(40, cider::ir::IRType::I32, t, {{30, 2}, {50, 4}}));
    ir.finishRet(3, 40, cider::ir::IRType::I32, t);
    ir.addBlock(4);
    ir.emit(ir.constant(50, 50, cider::ir::IRType::I32, t));
    ir.finishBr(4, 3);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,3,4,", "the merged block is gone");
    const cider::ir::Instr* phi = instructionWithResult(*ir.fn, 40);
    check(phi != nullptr && hasIncoming(*phi, 1, 30),
          "the phi entry now names the predecessor and keeps its value");
    check(phi != nullptr && !mentionsBlock(*phi, 2),
          "no phi entry still names the removed block");
    check(result.blocksMerged == 1, "one merge is counted");
}

void testMultiwordPhiRewrite() {
    std::printf("multiword phis relabel by block, not by position\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i64();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 4);
    ir.addBlock(1);
    ir.emit(ir.constant(20, 20, cider::ir::IRType::I64, t));
    ir.emit(ir.constant(21, 21, cider::ir::IRType::I64, t));
    ir.finishBr(1, 2);
    ir.addBlock(2);
    ir.emit(ir.constant(30, 30, cider::ir::IRType::I64, t));
    ir.emit(ir.constant(31, 31, cider::ir::IRType::I64, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    // The two words list their predecessors in opposite order on purpose.
    ir.emit(ir.phi(40, cider::ir::IRType::I64, t, {{30, 2}, {50, 4}}));
    ir.emit(ir.phi(41, cider::ir::IRType::I64, t, {{51, 4}, {31, 2}}));
    ir.emit(ir.binary(cider::ir::Op::Add, 42,
                      ir.value(40, cider::ir::IRType::I64, t),
                      ir.value(41, cider::ir::IRType::I64, t),
                      cider::ir::IRType::I64, t));
    ir.finishRet(3, 42, cider::ir::IRType::I64, t);
    ir.addBlock(4);
    ir.emit(ir.constant(50, 50, cider::ir::IRType::I64, t));
    ir.emit(ir.constant(51, 51, cider::ir::IRType::I64, t));
    ir.finishBr(4, 3);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,3,4,", "the merged block is gone");
    const cider::ir::Instr* first = instructionWithResult(*ir.fn, 40);
    const cider::ir::Instr* second = instructionWithResult(*ir.fn, 41);
    check(first != nullptr && hasIncoming(*first, 1, 30) &&
              hasIncoming(*first, 4, 50),
          "the first word keeps its own predecessor-value pairing");
    check(second != nullptr && hasIncoming(*second, 4, 51) &&
              hasIncoming(*second, 1, 31),
          "the reversed second word is relabelled independently");
    check(result.blocksMerged == 1, "one merge is counted");
}

void testEmptyPhiTargetAlreadyPredecessorSkipped() {
    std::printf("a phi target that already names the predecessor is kept\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.emit(ir.constant(5, 5, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(7, 7, cider::ir::IRType::I32, t));
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 3);
    ir.addBlock(1);
    ir.finishBr(1, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(20, cider::ir::IRType::I32, t, {{5, 1}, {7, 0}}));
    ir.finishRet(3, 20, cider::ir::IRType::I32, t);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,3,",
          "the empty block stays because its predecessor already reaches "
          "the phi block directly");
    check(result.emptyBlocksRemoved == 0 && result.blocksMerged == 0,
          "no counter moves");
}

void testEmptyBlockBeforePhisRewrite() {
    std::printf("one-predecessor empty block relabels the target phi\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.emit(ir.constant(5, 9, cider::ir::IRType::I32, t));
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.constant(10, 10, cider::ir::IRType::I32, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(20, cider::ir::IRType::I32, t, {{5, 1}, {10, 2}}));
    ir.finishRet(3, 20, cider::ir::IRType::I32, t);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,2,3,", "the empty block is gone");
    const cider::ir::Instr* phi = instructionWithResult(*ir.fn, 20);
    check(phi != nullptr && hasIncoming(*phi, 0, 5) &&
              hasIncoming(*phi, 2, 10),
          "the phi entry now names the surviving predecessor");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::CondBr &&
              ir.fn->blocks[0].term.thenTarget == 3,
          "the redirected edge reaches the phi block");
    check(result.emptyBlocksRemoved == 1 && result.blocksMerged == 0,
          "one empty removal is counted");
}

void testEliminationUpdatesPredecessors() {
    std::printf("elimination refreshes predecessors for a later merge\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.finishBr(0, 1);
    ir.addBlock(1);
    ir.finishBr(1, 2);
    ir.addBlock(2);
    ir.emit(ir.constant(5, 5, cider::ir::IRType::I32, t));
    ir.finishRet(2, 5, cider::ir::IRType::I32, t);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,",
          "the empty block is removed and the target then merges");
    check(result.emptyBlocksRemoved == 1 && result.blocksMerged == 1,
          "both transformations are counted");
}

void testSwitchPredecessorRedirect() {
    std::printf("a switch edge is redirected when its block is removed\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(1, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(2, 2, cider::ir::IRType::I32, t));
    ir.finishSwitch(0, ir.value(0, cider::ir::IRType::I32, t),
                    {{ir.value(1, cider::ir::IRType::I32, t), 1},
                     {ir.value(2, cider::ir::IRType::I32, t), 2}},
                    2);
    ir.addBlock(1);
    ir.emit(ir.constant(10, 10, cider::ir::IRType::I32, t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.constant(30, 30, cider::ir::IRType::I32, t));
    ir.finishRet(3, 30, cider::ir::IRType::I32, t);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,3,", "the empty switch target is gone");
    const cider::ir::Block* entry = blockWithLabel(*ir.fn, 0);
    check(entry != nullptr && entry->term.term == cider::ir::Term::Switch,
          "the terminator is still a switch");
    check(entry != nullptr && entry->term.cases.size() == 2 &&
              entry->term.cases[0].target == 1 &&
              entry->term.cases[1].target == 3 &&
              entry->term.defaultTarget == 3,
          "the matching case and the default now name the real target");
    check(result.emptyBlocksRemoved == 1, "one empty removal is counted");
}

void testAsmLabelPinsFunction() {
    std::printf("a function whose asm names a block label is untouched\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.asmBlock("jmp .L_f_1"));
    ir.finishBr(0, 1);
    ir.addBlock(1);
    ir.finishBr(1, 2);
    ir.addBlock(2);
    ir.emit(ir.constant(5, 5, cider::ir::IRType::I32, t));
    ir.finishRet(2, 5, cider::ir::IRType::I32, t);

    cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,2,",
          "the block named by assembly keeps its label");
    check(result.emptyBlocksRemoved == 0 && result.blocksMerged == 0,
          "the whole function is skipped when assembly names a label");
}

void testPinnedEffectsAreNotMerged() {
    std::printf("asm, trap and volatile blocks are never merged\n");
    {
        Ir ir;
        ir.finishBr(0, 1);
        ir.addBlock(1);
        ir.emit(ir.asmBlock());
        ir.finishVoidRet(1);

        cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
        check(structurallyValid(ir.module), "the asm block stays valid");
        check(blockLabels(*ir.fn) == "0,1,", "the asm block is not merged");
        check(result.blocksMerged == 0, "no merge is counted");
    }
    {
        Ir ir;
        ir.finishBr(0, 1);
        ir.addBlock(1);
        ir.emit(ir.trap());
        ir.finishVoidRet(1);

        cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
        check(structurallyValid(ir.module), "the trap block stays valid");
        check(blockLabels(*ir.fn) == "0,1,", "the trap body is not moved");
        check(result.blocksMerged == 0, "no merge is counted");
    }
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.constant(2, 7, cider::ir::IRType::I32, t));
        ir.emit(ir.constant(3, 9, cider::ir::IRType::I32, t));
        ir.finishBr(0, 1);
        ir.addBlock(1);
        ir.emit(ir.store(1, ir.value(2, cider::ir::IRType::I32, t),
                         cider::ir::IRType::I32, t, true));
        ir.finishRet(1, 3, cider::ir::IRType::I32, t);

        cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
        check(structurallyValid(ir.module), "the volatile block stays valid");
        check(blockLabels(*ir.fn) == "0,1,",
              "the volatile store is not moved");
        check(result.blocksMerged == 0, "no merge is counted");
    }
    {
        Ir ir;
        ir.finishBr(0, 1);
        ir.addBlock(1);
        ir.finishTrap(1);

        cider::opt::BlocksResult result = cider::opt::cleanupBlocks(ir.module);
        check(structurallyValid(ir.module), "the trap terminator stays valid");
        check(blockLabels(*ir.fn) == "0,1,",
              "a trap terminator is not moved");
        check(result.blocksMerged == 0, "no merge is counted");
    }
}

// A nested chain used twice: two empty blocks before a phi join, and a
// straight-line tail that merges. The description and counters must be equal
// for independently built modules.
void buildShapeModule(Ir& ir) {
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.emit(ir.constant(1, 9, cider::ir::IRType::I32, t));
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(10, 10, cider::ir::IRType::I32, t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.constant(11, 11, cider::ir::IRType::I32, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(20, cider::ir::IRType::I32, t, {{10, 1}, {11, 2}}));
    ir.finishBr(3, 4);
    ir.addBlock(4);
    ir.emit(ir.constant(30, 30, cider::ir::IRType::I32, t));
    ir.finishBr(4, 5);
    ir.addBlock(5);
    ir.emit(ir.constant(31, 31, cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Add, 40,
                      ir.value(20, cider::ir::IRType::I32, t),
                      ir.value(30, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Add, 41,
                      ir.value(40, cider::ir::IRType::I32, t),
                      ir.value(31, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.finishRet(5, 41, cider::ir::IRType::I32, t);
}

void testDeterminism() {
    std::printf("two identical modules clean identically and are idempotent\n");
    Ir first;
    buildShapeModule(first);
    Ir second;
    buildShapeModule(second);
    cider::opt::BlocksResult firstResult =
        cider::opt::cleanupBlocks(first.module);
    cider::opt::BlocksResult secondResult =
        cider::opt::cleanupBlocks(second.module);
    check(structurallyValid(first.module), "the first result is valid");
    check(structurallyValid(second.module), "the second result is valid");
    check(describe(*first.fn) == describe(*second.fn),
          "the two functions have the same shape");
    check(cider::ir::dumpModule(first.module) ==
              cider::ir::dumpModule(second.module),
          "two identical modules dump byte-identically");
    check(firstResult.emptyBlocksRemoved == secondResult.emptyBlocksRemoved &&
              firstResult.blocksMerged == secondResult.blocksMerged,
          "the counters do not depend on insertion order");
    check(firstResult.blocksMerged == 2,
          "the two straight-line tail blocks merge in one run");
    check(blockLabels(*first.fn) == "0,1,2,3,",
          "the surviving order is the input order without the tail");

    cider::opt::BlocksResult again =
        cider::opt::cleanupBlocks(first.module);
    check(again.emptyBlocksRemoved == 0 && again.blocksMerged == 0,
          "a second run is a no-op");
}

void testPipelineResult() {
    std::printf("the pipeline reports the block-cleanup counters\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.finishBr(0, 1);
    ir.addBlock(1);
    ir.emit(ir.constant(1, 3, cider::ir::IRType::I32, t));
    ir.finishBr(1, 2);
    ir.addBlock(2);
    ir.emit(ir.constant(2, 4, cider::ir::IRType::I32, t));
    ir.finishRet(2, 2, cider::ir::IRType::I32, t);

    cider::opt::OptResult result = cider::opt::run(ir.module);
    check(result.ok, "the pipeline succeeds: " + result.error);
    check(structurallyValid(ir.module), "the pipeline result is valid");
    check(result.blocksMerged == 2,
          "both merge counters reach OptResult");
    check(blockLabels(*ir.fn) == "0,", "the chain collapses to the entry");
}

}  // namespace

int main() {
    std::printf("blocks_test — empty blocks and single-predecessor merges\n\n");
    testEmptyBlockOnePredecessor();
    testEmptyBlockMultiplePredecessors();
    testMultiplePredecessorsWithTargetPhisSkipped();
    testSinglePredecessorMerge();
    testMultiplePredecessorsMergeSkipped();
    testEntryPreserved();
    testSelfLoopsSkipped();
    testPhiIncomingRewriteOnMerge();
    testMultiwordPhiRewrite();
    testEmptyPhiTargetAlreadyPredecessorSkipped();
    testEmptyBlockBeforePhisRewrite();
    testEliminationUpdatesPredecessors();
    testSwitchPredecessorRedirect();
    testAsmLabelPinsFunction();
    testPinnedEffectsAreNotMerged();
    testDeterminism();
    testPipelineResult();
    std::printf("\n%d checks, %s\n", checks,
                failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
