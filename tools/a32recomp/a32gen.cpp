// a32gen: turn instruction sequences into C block functions for the test harness.
//
// stdin, one case per line:  <name> <base_hex> <word_hex> [<word_hex> ...]
// writes a C file (argv[1]) with `void <name>(A32Context*)` per case and a
// table `a32_cases[]`; prints "<name> ok|unhandled" per case to stdout.
#include "a32_to_c.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: a32gen out.c < cases\n"); return 2; }
    std::ofstream out(argv[1]);
    out << "#include \"a32_runtime.h\"\n\n";
    std::string line;
    std::vector<std::string> names;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string name, basehex, w;
        if (!(ss >> name >> basehex)) continue;
        uint32_t pc = (uint32_t)std::stoul(basehex, nullptr, 16);
        std::vector<uint32_t> words;
        while (ss >> w) words.push_back((uint32_t)std::stoul(w, nullptr, 16));
        std::string body;
        bool open = true, any_unhandled = false;
        for (uint32_t k = 0; k < words.size() && open; ++k) {
            bool u = false;
            open = a32recomp::Translate(words[k], pc, body, &u);
            any_unhandled |= u;
            pc += 4;
        }
        if (open) body += a32recomp::F("    c->r[15] = 0x%08Xu; return;\n", pc);
        out << "void " << name << "(A32Context* c) {\n" << body << "}\n";
        names.push_back(name);
        std::cout << name << (any_unhandled ? " unhandled" : " ok") << "\n";
    }
    out << "\nconst A32BlockFn a32_cases[] = {\n";
    for (auto& n : names) out << "    " << n << ",\n";
    out << "};\nconst unsigned a32_case_count = " << names.size() << ";\n";
}
