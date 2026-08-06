#include "ir_text.h"

#include <cstdlib>
#include <sstream>

namespace cider {

namespace {

struct Tokenizer {
    const std::string& text;
    int line = 0;
    size_t pos = 0;

    bool nextLine(std::string& out) {
        while (pos < text.size()) {
            ++line;
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) eol = text.size();
            std::string raw = text.substr(pos, eol - pos);
            pos = (eol < text.size()) ? eol + 1 : eol;
            size_t semi = raw.find(';');
            if (semi != std::string::npos) raw = raw.substr(0, semi);
            size_t b = raw.find_first_not_of(" \t");
            size_t e = raw.find_last_not_of(" \t");
            if (b == std::string::npos) continue;
            out = raw.substr(b, e - b + 1);
            return true;
        }
        return false;
    }
};

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool inStr = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') { inStr = !inStr; cur += c; continue; }
        if (!inStr && (c == ' ' || c == ',' || c == '\t')) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() - 1) {
            char n = s[i + 1];
            if (n == 'n') { out += '\n'; ++i; continue; }
            if (n == 't') { out += '\t'; ++i; continue; }
            if (n == '\\') { out += '\\'; ++i; continue; }
            if (n == '"') { out += '"'; ++i; continue; }
        }
        out += s[i];
    }
    return out;
}

}  // namespace

Program parseIR(const std::string& text) {
    Program prog;
    Tokenizer tok{text, 0, 0};
    std::string line;
    Function* cur = nullptr;

    while (tok.nextLine(line)) {
        std::vector<std::string> w = splitWords(line);
        if (w.empty()) continue;
        if (w[0] == "func") {
            if (w.size() != 2 || w[1].back() != ':') throw ParseError{"malformed func line", tok.line};
            Function fn;
            fn.name = w[1].substr(0, w[1].size() - 1);
            prog.functions.push_back(std::move(fn));
            cur = &prog.functions.back();
            continue;
        }
        if (!cur) throw ParseError{"instruction outside func", tok.line};

        auto operand = [&](const std::string& s) -> Operand {
            Operand o;
            if (!s.empty() && s[0] == '%') { o.isReg = true; o.reg = s; }
            else {
                o.isReg = false;
                o.imm = std::strtoll(s.c_str(), nullptr, 10);
            }
            return o;
        };

        Instruction instr;
        instr.line = tok.line;
        size_t idx = 0;
        if (w[0][0] == '%') {           // %dest = op ...
            instr.dest = w[0];
            if (w.size() < 3 || w[1] != "=") throw ParseError{"expected '=' after dest", tok.line};
            idx = 2;
        }
        std::string opName = w[idx++];
        if (opName == "const") {
            if (idx >= w.size()) throw ParseError{"const needs an operand", tok.line};
            instr.op = Op::CONST;
            instr.args.push_back(operand(w[idx++]));
        } else if (opName == "str") {
            if (idx >= w.size() || w[idx].size() < 2 || w[idx].front() != '"')
                throw ParseError{"str needs a string literal", tok.line};
            prog.strings.push_back(unescape(w[idx++]));
            instr.op = Op::STR;
            Operand o; o.imm = static_cast<int64_t>(prog.strings.size() - 1);
            instr.args.push_back(o);
        } else if (opName == "write") {
            if (w.size() - idx != 3) throw ParseError{"write needs fd, ptr, len", tok.line};
            instr.op = Op::WRITE;
            instr.args.push_back(operand(w[idx++]));
            instr.args.push_back(operand(w[idx++]));
            instr.args.push_back(operand(w[idx++]));
        } else if (opName == "exit") {
            if (idx >= w.size()) throw ParseError{"exit needs a code", tok.line};
            instr.op = Op::EXIT;
            instr.args.push_back(operand(w[idx++]));
        } else {
            throw ParseError{"unknown op: " + opName, tok.line};
        }
        if (idx != w.size()) throw ParseError{"trailing tokens", tok.line};
        cur->instrs.push_back(std::move(instr));
    }
    return prog;
}

}  // namespace cider
