// codegen.cpp — checked IR to x86-64 assembly (SysV, Linux).
//
// Intel syntax with `.intel_syntax noprefix`, so plain `cc file.s` assembles
// it. Every SSA value lives in its own stack slot and six scratch registers
// carry values between slot and operation: correct by construction, slow by
// design. A register allocator replaces this strategy, not its interface.
//
// The text is a pure function of the module: no timestamps, no working
// directories, no absolute paths. Gate L5.

#include "codegen/codegen.h"

namespace cider {
namespace codegen {

namespace {

const char* argReg(uint64_t i) {
    if (i == 0) {
        return "rdi";
    }
    if (i == 1) {
        return "rsi";
    }
    if (i == 2) {
        return "rdx";
    }
    if (i == 3) {
        return "rcx";
    }
    if (i == 4) {
        return "r8";
    }
    return "r9";
}

bool signedWidth(ir::IRType width, TypeRef type) {
    if (width == ir::IRType::I8 || width == ir::IRType::I16 || width == ir::IRType::I32 ||
        width == ir::IRType::I64) {
        return true;
    }
    if (width == ir::IRType::U8 || width == ir::IRType::U16 || width == ir::IRType::U32 ||
        width == ir::IRType::U64) {
        return false;
    }
    if (type.isError() || type.type->kind() != Kind::Basic) {
        return true;
    }
    return basicIsSigned(*type.type->basic());
}

const char* setFor(ir::CmpOp cmp) {
    if (cmp == ir::CmpOp::Eq) {
        return "sete";
    }
    if (cmp == ir::CmpOp::Ne) {
        return "setne";
    }
    if (cmp == ir::CmpOp::Lt) {
        return "setl";
    }
    if (cmp == ir::CmpOp::Ult) {
        return "setb";
    }
    if (cmp == ir::CmpOp::Le) {
        return "setle";
    }
    if (cmp == ir::CmpOp::Ule) {
        return "setbe";
    }
    if (cmp == ir::CmpOp::Gt) {
        return "setg";
    }
    if (cmp == ir::CmpOp::Ugt) {
        return "seta";
    }
    if (cmp == ir::CmpOp::Ge) {
        return "setge";
    }
    return "setae";
}

}  // namespace

class Emitter {
public:
    explicit Emitter(const ir::Module& module) : module_(module) {}

    EmitResult run() {
        out_ += ".intel_syntax noprefix\n.text\n";
        for (const ir::Function& fn : module_.functions) {
            if (!emitFunction(fn)) {
                return EmitResult{false, "", error_, pos_};
            }
        }
        if (!module_.globals.empty()) {
            out_ += ".data\n";
            for (const ir::Global& global : module_.globals) {
                out_ += global.label + ":\n";
                if (global.initLabel.empty()) {
                    out_ += "  .quad " + std::to_string(global.initial) + "\n";
                } else {
                    // A string global is its bytes' address followed by its
                    // length — the same two words a literal lowers to.
                    out_ += "  .quad " + global.initLabel + "\n";
                    out_ += "  .quad " + std::to_string(global.initial) + "\n";
                }
            }
        }
        if (!module_.rodata.empty()) {
            // Bytes in first-seen order: the order is the lowering order,
            // which is deterministic in the source, so the bytes are too.
            out_ += ".section .rodata\n";
            for (const ir::Rodata& blob : module_.rodata) {
                out_ += blob.label + ":\n";
                if (!blob.bytes.empty()) {
                    std::string line = "  .byte ";
                    for (unsigned char byte : blob.bytes) {
                        line += std::to_string(byte) + ",";
                    }
                    line.back() = '\n';
                    out_ += line;
                }
            }
        }
        return EmitResult{true, out_, "", Position{"", 1, 1}};
    }

private:
    void refuse(Position pos, const std::string& what) {
        if (failed_) {
            return;
        }
        failed_ = true;
        error_ = what;
        pos_ = pos;
    }

    // One slot per value id, eight bytes each. Slots hold the value extended
    // to 64 bits (sign or zero per the type), so every load is a full
    // 64-bit read and no consumer ever guesses a slot's width — the defect
    // v2's spill slots had.
    std::string slot(uint32_t id) const {
        return "[rsp + " + std::to_string(id * 8) + "]";
    }

    std::string labelFor(const std::string& fn, uint32_t label) const {
        return ".L_" + fn + "_" + std::to_string(label);
    }

    bool emitFunction(const ir::Function& fn) {
        // The frame covers every value id the function mentions, rounded up
        // so `sub rsp` keeps 16-byte alignment for the next call.
        uint32_t top = 0;
        auto note = [&](uint32_t id) {
            top = id > top ? id : top;
        };
        for (const ir::Value& param : fn.params) {
            note(param.id);
        }
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                for (const ir::Value& result : instr.results) {
                    note(result.id);
                }
                for (const ir::Value& operand : instr.operands) {
                    note(operand.id);
                }
                for (const ir::Argument& arg : instr.arguments) {
                    for (const ir::Value& word : arg.words) {
                        note(word.id);
                    }
                }
            }
            const ir::Terminator& term = block.term;
            if (term.value.has_value()) {
                note(term.value->id);
            }
            if (term.cond.has_value()) {
                note(term.cond->id);
            }
        }
        uint32_t cells = top + 1;
        uint32_t span = (cells * 8 + 15) & ~15u;

        out_ += ".globl " + fn.name + "\n";
        out_ += fn.name + ":\n";
        out_ += "  push rbp\n";
        out_ += "  mov rbp, rsp\n";
        out_ += "  sub rsp, " + std::to_string(span) + "\n";

        // Parameters arrive in registers (then stack); they live in slots
        // from here on, like every other value.
        uint64_t index = 0;
        for (const ir::Value& param : fn.params) {
            if (index < 6) {
                out_ += "  mov " + slot(param.id) + ", " + argReg(index) + "\n";
            } else {
                uint64_t stackAt = 16 + (index - 6) * 8;
                out_ += "  mov rax, [rbp + " + std::to_string(stackAt) + "]\n";
                out_ += "  mov " + slot(param.id) + ", rax\n";
            }
            ++index;
        }

        for (const ir::Block& block : fn.blocks) {
            out_ += labelFor(fn.name, block.label) + ":\n";
            for (const ir::Instr& instr : block.body) {
                if (!emitInstr(fn, instr)) {
                    return false;
                }
            }
            if (!emitTerm(fn, block.term)) {
                return false;
            }
        }
        return !failed_;
    }

    bool emitInstr(const ir::Function& fn, const ir::Instr& instr) {
        if (instr.op == ir::Op::Const) {
            // 32-bit immediates encode directly; anything wider materialises
            // through rax, because x86 has no 64-bit-immediate-to-memory mov.
            if (instr.constantBits >= INT32_MIN && instr.constantBits <= INT32_MAX) {
                out_ += "  mov rax, " + std::to_string(instr.constantBits) + "\n";
            } else {
                out_ += "  movabs rax, " + std::to_string(instr.constantBits) + "\n";
            }
            if (!instr.results.empty()) {
                out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            }
            return true;
        }
        if (instr.op == ir::Op::Copy) {
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Add || instr.op == ir::Op::Sub ||
            instr.op == ir::Op::And || instr.op == ir::Op::Or ||
            instr.op == ir::Op::Xor || instr.op == ir::Op::Mul) {
            const char* name = "add";
            if (instr.op == ir::Op::Sub) {
                name = "sub";
            } else if (instr.op == ir::Op::And) {
                name = "and";
            } else if (instr.op == ir::Op::Or) {
                name = "or";
            } else if (instr.op == ir::Op::Xor) {
                name = "xor";
            } else if (instr.op == ir::Op::Mul) {
                name = "imul";
            }
            // 64-bit wraps naturally, so Wrap and Undefined emit the same
            // instruction: the mode travelled on the IR for the optimiser,
            // not for a second opcode here.
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += std::string("  ") + name + " rax, " + slot(instr.operands[1].id) + "\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Div || instr.op == ir::Op::Rem) {
            // Signed divides sign-extend; unsigned ones zero the top instead.
            // Either faults on a zero divisor, which is the defined trap.
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            if (signedWidth(instr.width, instr.type)) {
                out_ += "  cqo\n";
                out_ += "  idiv qword ptr " + slot(instr.operands[1].id) + "\n";
            } else {
                out_ += "  xor edx, edx\n";
                out_ += "  div qword ptr " + slot(instr.operands[1].id) + "\n";
            }
            if (instr.op == ir::Op::Div) {
                out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            } else {
                out_ += "  mov " + slot(instr.results[0].id) + ", rdx\n";
            }
            return true;
        }
        if (instr.op == ir::Op::Shl || instr.op == ir::Op::Shr) {
            // The count lives in cl: x86 masks it to 5 or 6 bits itself,
            // which is SEMANTICS §3's documented result, not an accident.
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  mov rcx, " + slot(instr.operands[1].id) + "\n";
            if (instr.op == ir::Op::Shl) {
                out_ += "  shl rax, cl\n";
            } else if (signedWidth(instr.width, instr.type)) {
                out_ += "  sar rax, cl\n";
            } else {
                out_ += "  shr rax, cl\n";
            }
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Neg) {
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  neg rax\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::BitNot) {
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  not rax\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Not) {
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  test rax, rax\n";
            out_ += "  setz al\n";
            out_ += "  movzx rax, al\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Cmp) {
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  cmp rax, " + slot(instr.operands[1].id) + "\n";
            out_ += std::string("  ") + setFor(instr.cmp) + " al\n";
            out_ += "  movzx rax, al\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        // A load or store names its place two ways: a global carries its link
        // label, and a slot carries its id, whose address is rsp+id*8 by the
        // same construction the frame uses. One branch, decided by which the
        // instruction names.
        if (instr.op == ir::Op::Load) {
            std::string place = instr.globalLabel.empty()
                                    ? slot(instr.addressBase.id)
                                    : "[rip + " + instr.globalLabel + "]";
            out_ += "  mov rax, " + place + "\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Store) {
            std::string place = instr.globalLabel.empty()
                                    ? slot(instr.addressBase.id)
                                    : "[rip + " + instr.globalLabel + "]";
            out_ += "  mov rax, " + slot(instr.storedValue.id) + "\n";
            out_ += "  mov " + place + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Alloca) {
            // No code: the slot's address is rsp+id*8 by construction, so every
            // load and store already knows where it lives.
            return true;
        }
        if (instr.op == ir::Op::Call) {
            // Words, not arguments: a string occupies two registers, so six
            // string arguments would silently share r9 without this count.
            uint64_t arity = 0;
            for (const ir::Argument& arg : instr.arguments) {
                for (const ir::Value& word : arg.words) {
                    (void)word;
                    ++arity;
                }
            }
            const uint64_t regArgs = 6;
            if (arity > regArgs) {
                refuse(instr.loc, "more than six registers of arguments");
                return false;
            }
            // Words flatten in order: a string arrives as (data, length) in
            // (rdi, rsi), which is the SysV two-eightbyte rule and exactly
            // what the C struct on the other side declares.
            uint64_t index = 0;
            for (const ir::Argument& arg : instr.arguments) {
                for (const ir::Value& word : arg.words) {
                    out_ += "  mov " + std::string(argReg(index)) + ", " +
                            slot(word.id) + "\n";
                    ++index;
                }
            }
            if (instr.call.indirect) {
                out_ += "  mov rax, " + slot(instr.calleeValue.id) + "\n";
                out_ += "  call rax\n";
            } else {
                out_ += "  call " + instr.callee + "\n";
            }
            if (!instr.results.empty()) {
                out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            }
            return true;
        }
        if (instr.op == ir::Op::Cast) {
            return emitCast(instr);
        }
        if (instr.op == ir::Op::Trap) {
            out_ += "  ud2\n";
            return true;
        }
        if (instr.op == ir::Op::DataAddr) {
            out_ += "  lea rax, [rip + " + instr.globalLabel + "]\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        refuse(instr.loc, "this instruction is not emitted yet");
        return false;
    }

    // Integer casts only: same size is a move, narrowing truncates and
    // re-extends, widening extends. Floats and address casts refuse with a
    // diagnostic naming the gap rather than a wrong move.
    bool emitCast(const ir::Instr& instr) {
        uint64_t sources = 0;
        for (const ir::Value& operand : instr.operands) {
            (void)operand;
            ++sources;
        }
        const uint64_t want = 1;
        if (sources != want || instr.results.empty()) {
            refuse(instr.loc, "a bad cast");
            return false;
        }
        TypeRef from = instr.operands[0].type;
        TypeRef to = instr.type;
        if (from.isError() || to.isError() || from.type->kind() != Kind::Basic ||
            to.type->kind() != Kind::Basic) {
            refuse(instr.loc, "this cast is not emitted yet");
            return false;
        }
        BasicKind a = *from.type->basic();
        BasicKind b = *to.type->basic();
        if (basicIsFloat(a) || basicIsFloat(b)) {
            refuse(instr.loc, "float casts are not emitted yet");
            return false;
        }
        int fromBits = basicWidthBits(a);
        int toBits = basicWidthBits(b);
        out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
        if (fromBits == toBits) {
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        const char* move = basicIsSigned(b) ? "movsx" : "movzx";
        std::string reg = "rax";
        if (toBits == 8) {
            reg = std::string(move) + " rax, al";
        } else if (toBits == 16) {
            reg = std::string(move) + " rax, ax";
        } else if (toBits == 32) {
            reg = basicIsSigned(b) ? "movsxd rax, eax" : "mov eax, eax";
        } else {
            reg = "mov rax, rax";
        }
        // Narrowing first truncates through the source width: a 64-bit value
        // narrowed to u8 keeps its low byte, whatever the high bits held.
        if (fromBits > toBits) {
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
        }
        out_ += "  " + reg + "\n";
        out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
        return true;
    }

    bool emitTerm(const ir::Function& fn, const ir::Terminator& term) {
        if (term.term == ir::Term::Ret) {
            if (term.value.has_value() && term.value->id != 0xFFFFFFFFu) {
                out_ += "  mov rax, " + slot(term.value->id) + "\n";
            }
            out_ += "  leave\n";
            out_ += "  ret\n";
            return true;
        }
        if (term.term == ir::Term::Br) {
            out_ += "  jmp " + labelFor(fn.name, term.target) + "\n";
            return true;
        }
        if (term.term == ir::Term::CondBr && term.cond.has_value()) {
            out_ += "  mov rax, " + slot(term.cond->id) + "\n";
            out_ += "  test rax, rax\n";
            out_ += "  jnz " + labelFor(fn.name, term.thenTarget) + "\n";
            out_ += "  jmp " + labelFor(fn.name, term.elseTarget) + "\n";
            return true;
        }
        if (term.term == ir::Term::Unreachable) {
            // Provably unreached, so a trap is the honest emission: silence
            // would fall through into whatever block follows.
            out_ += "  ud2\n";
            return true;
        }
        refuse(term.loc, "this terminator is not emitted yet");
        return false;
    }





    const ir::Module& module_;
    std::string out_;
    bool failed_ = false;
    std::string error_;
    Position pos_{"", 1, 1};
};

const char* targetName(Target target) {
    if (target == Target::Native) {
        return "native";
    }
    if (target == Target::Pomeos) {
        return "pomeos";
    }
    if (target == Target::Uefi) {
        return "uefi";
    }
    return "vm";
}

EmitResult emitNative(const ir::Module& module) {
    Emitter emitter(module);
    return emitter.run();
}

EmitResult emit(const ir::Module& module, Target target) {
    if (target != Target::Native) {
        EmitResult denied;
        denied.ok = false;
        denied.error = std::string("target ") + targetName(target) +
                       " is not emitted yet: native only in this slice";
        return denied;
    }
    return emitNative(module);
}

}  // namespace codegen
}  // namespace cider
