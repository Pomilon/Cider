// main.cpp — the `cider` entry point and its one catch.
//
// Everything a pass can diagnose travels as a value to stderr with an exit
// code; this catch exists for what cannot: I/O failures, argument failures,
// and anything thrown. A raw exception escaping a pass would end in
// std::terminate with no diagnostic, which is what gate L6 forbids — so the
// boundary maps every failure to text and a nonzero exit.

#include <cstdio>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "driver/driver.h"

int main(int argc, char** argv) {
    try {
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            args.push_back(argv[i]);
        }
        std::string out;
        std::string err;
        int code = cider::driver::runDriver(args, out, err);
        if (!out.empty()) {
            std::fwrite(out.data(), 1, out.size(), stdout);
        }
        if (!err.empty()) {
            std::fwrite(err.data(), 1, err.size(), stderr);
        }
        return code;
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "compiler: out of memory\n");
        return 2;
    } catch (const std::length_error&) {
        std::fprintf(stderr, "compiler: out of memory\n");
        return 2;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "cider: error: %s\n", failure.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "cider: error: unknown failure\n");
        return 1;
    }
}
