#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::printf("cider 0.2.0\n");
        return 0;
    }
    std::fprintf(stderr, "usage: cider <command> [options] <file>\n");
    return 2;
}
