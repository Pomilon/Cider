#pragma once

#include <string>

namespace cider {

// Last segment of a dotted logical module path: "a.util" -> "util".
inline std::string qualifierForModule(const std::string& logical) {
    auto p = logical.find_last_of('.');
    return (p == std::string::npos) ? logical : logical.substr(p + 1);
}

// Escape underscores inside one path/name segment so the mangling below is
// injective: every '_' becomes '_u'. Underscores in escaped output are
// therefore always immediately followed by 'u', so escaped output never
// contains '__' and '__' in a mangled name uniquely marks separators.
inline std::string escapeModuleSegment(const std::string& segment) {
    std::string out;
    out.reserve(segment.size());
    for (char c : segment) {
        if (c == '_')
            out += "_u";
        else
            out += c;
    }
    return out;
}

// Emit-only IR mangling (spec section 5): dots in the module path become
// double underscores, e.g. module "a.util" + "foo" -> "mod_a__util__foo".
// Never parsed back; main-file symbols keep plain names and `extern` /
// `_start` are exempt (handled by later tasks). The underscore escaping
// keeps distinct (logical, name) pairs distinct: without it, module "a_b"
// + "c" and module "a" + "b_c" would both mangle to "mod_a_b_c"; with it
// they give "mod_a_ub__c" vs "mod_a__b_uc". Since escaped segments never
// contain '__', every '__' in the output is a separator (dot or
// module/name boundary), so the mangled name decodes to exactly one
// (logical, name) pair. A user identifier would have to deliberately
// mimic the `mod_` prefix with `__` separators and `_u` runs to collide.
inline std::string mangleQualifiedName(const std::string& logical, const std::string& name) {
    std::string out = "mod_";
    bool first = true;
    size_t start = 0;
    while (true) {
        size_t dot = logical.find('.', start);
        std::string seg = (dot == std::string::npos) ? logical.substr(start)
                                                     : logical.substr(start, dot - start);
        if (!first) out += "__";
        first = false;
        out += escapeModuleSegment(seg);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    out += "__";
    out += escapeModuleSegment(name);
    return out;
}

// Canonical type identity: the FULL logical module path + "::" + name
// (e.g. "a.util::Foo"), never the bare qualifier, so same-last-segment
// modules ("a.util" vs "b.util") stay distinct. Already-qualified names
// pass through; the empty module (main file) keeps the legacy bare name.
inline std::string canonicalTypeName(const std::string& name, const std::string& currentModule) {
    if (name.find("::") != std::string::npos) return name;  // already canonical
    if (currentModule.empty()) return name;                 // main file: legacy identity
    return currentModule + "::" + name;
}

}  // namespace cider
