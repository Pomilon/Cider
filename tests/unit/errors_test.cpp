// CIDER-FEAT-005: ErrorReporter severity/collection unit test.
//
// Errors and warnings are collected separately: warnings never make
// hasErrors() true, so an opt-in --warn build cannot change an exit code.
#include <cstdio>
#include <string>
#include <vector>

#include "errors.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int run() {
    cider::ErrorReporter rep;
    rep.setFile("main.cid");

    CHECK(!rep.hasErrors());
    CHECK(!rep.hasWarnings());
    CHECK(rep.errors().empty());
    CHECK(rep.warnings().empty());

    // Warnings carry Warning severity and the reporter's current file/line.
    rep.warning("unused variable 'x'", 4, 9);
    rep.warning("unused import 'f' from module 'm'", 1);
    CHECK(rep.hasWarnings());
    CHECK(!rep.hasErrors());
    CHECK(rep.errors().empty());
    CHECK(rep.warnings().size() == 2);
    CHECK(rep.warnings()[0].severity == cider::Diagnostic::Severity::Warning);
    CHECK(rep.warnings()[0].message == "unused variable 'x'");
    CHECK(rep.warnings()[0].line == 4);
    CHECK(rep.warnings()[0].col == 9);
    CHECK(rep.warnings()[0].filename == "main.cid");
    CHECK(rep.warnings()[1].col == 0);

    // Errors keep their historical behaviour and severity.
    rep.error("Undefined identifier 'y'", 7, 3);
    CHECK(rep.hasErrors());
    CHECK(rep.errors().size() == 1);
    CHECK(rep.errors()[0].severity == cider::Diagnostic::Severity::Error);
    CHECK(rep.errors()[0].line == 7);

    // All-diagnostics access keeps collection order (errors, then warnings).
    std::vector<cider::Diagnostic> all = rep.diagnostics();
    CHECK(all.size() == 3);
    CHECK(all[0].severity == cider::Diagnostic::Severity::Error);
    CHECK(all[1].severity == cider::Diagnostic::Severity::Warning);
    CHECK(all[2].severity == cider::Diagnostic::Severity::Warning);

    // clear() drops both lists.
    rep.clear();
    CHECK(!rep.hasErrors());
    CHECK(!rep.hasWarnings());
    CHECK(rep.diagnostics().empty());
    return 0;
}

int main() { return run(); }
