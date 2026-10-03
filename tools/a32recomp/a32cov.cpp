// a32cov: run code discovery + the A32 lifter over an uncompressed NSO and report coverage.
//
//   a32cov <module.nso> [--emit <dir> [--files N] [--name mod] [--rt runtime_dir]] [--conservative]
//          [--sweep N] [--misses recomp_misses.txt <module name>]
//
// --sweep sets Discover's boundary sweep level. --misses scores discovery against the
// block misses a run of the exported game recorded for this module (addresses the game is
// known to execute): how many became block starts, plain code (entered mid-block), data, or
// stayed unreached.
// --emit writes the recompiled C (blocks split across N files plus a sorted
// dispatch table), the same shape suyu's exporter produces for AArch64.
#include "a32emit.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>

using namespace a32recomp;

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: a32cov module.nso [--emit dir [--files N]]\n"); return 2; }
    std::string emit_dir, dump_kind, rt_dir = ".", mod_name, misses_file, misses_module;
    int sweep = 0;
    size_t unh_limit = 40;
    unsigned nfiles = 64;
    for (int k = 2; k < argc; ++k) {
        if (std::string(argv[k]) == "--emit" && k + 1 < argc) emit_dir = argv[++k];
        else if (std::string(argv[k]) == "--files" && k + 1 < argc) nfiles = (unsigned)atoi(argv[++k]);
        else if (std::string(argv[k]) == "--dump-kind" && k + 1 < argc) dump_kind = argv[++k];
        else if (std::string(argv[k]) == "--rt" && k + 1 < argc) rt_dir = argv[++k];
        else if (std::string(argv[k]) == "--name" && k + 1 < argc) mod_name = argv[++k];
        else if (std::string(argv[k]) == "--sweep" && k + 1 < argc) sweep = atoi(argv[++k]);
        else if (std::string(argv[k]) == "--unh" && k + 1 < argc) unh_limit = (size_t)atoi(argv[++k]);
        else if (std::string(argv[k]) == "--misses" && k + 2 < argc) {
            misses_file = argv[++k];
            misses_module = argv[++k];
        }
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<u8> file((std::istreambuf_iterator<char>(f)), {});
    NsoImage img;
    if (!LoadNso(file, img)) { fprintf(stderr, "not an uncompressed NSO\n"); return 1; }
    bool conservative = false;
    for (int k = 2; k < argc; ++k) if (std::string(argv[k]) == "--conservative") conservative = true;
    Discovery d = Discover(img, !conservative, nullptr, sweep);

    if (!misses_file.empty()) {
        std::ifstream mf(misses_file);
        std::set<u32> offs;
        size_t index;
        std::string off, name;
        while (mf >> index >> off >> name) {
            if (name == misses_module) offs.insert((u32)std::stoul(off, nullptr, 16));
        }
        size_t starts = 0, code = 0, data = 0, unreached = 0;
        for (const u32 o : offs) {
            if (!img.InText(o)) continue;
            const u8 k = d.kind[(o - img.text_off) / 4];
            if (d.block_starts.count(o)) starts++;
            else if (k == Discovery::Code) {
                if (code++ < 12)
                    printf("  mid-block %08X: %08X %08X [%08X]\n", o, img.Word(o - 8), img.Word(o - 4), img.Word(o));
            } else if (k == Discovery::Data) data++;
            else unreached++;
        }
        printf("misses (%zu known-executed addresses): block start %zu, code mid-block %zu, "
               "data %zu, unreached %zu\n", offs.size(), starts, code, data, unreached);
    }

    if (!dump_kind.empty()) {
        std::ofstream o(dump_kind, std::ios::binary);
        o.write((const char*)d.kind.data(), (std::streamsize)d.kind.size());
    }
    size_t code = 0, data = 0, unknown = 0;
    for (u8 k : d.kind) (k == Discovery::Code ? code : k == Discovery::Data ? data : unknown)++;
    const size_t words = d.kind.size();
    printf(".text: %.2f MB, %zu words\n", img.text_size / 1e6, words);
    printf("roots:");
    for (auto& [k, v] : d.roots) printf(" %s=%zu", k.c_str(), v);
    printf("\nclassified: code %zu (%.2f%%), literal/data %zu (%.2f%%), unreached %zu (%.2f%%)\n", code,
           100.0 * code / words, data, 100.0 * data / words, unknown, 100.0 * unknown / words);
    printf("blocks: %zu, thumb targets (not walked): %zu, walks stopped at untranslatable insn: %zu\n",
           d.block_starts.size(), d.thumb_targets.size(), d.unhandled_stops);

    // Translate every discovered code word and tally what falls back.
    size_t handled = 0, unhandled = 0;
    std::map<u32, std::pair<size_t, std::pair<u32, u32>>> sig; // key -> count, (example insn, pc)
    for (size_t w = 0; w < words; ++w) {
        if (d.kind[w] != Discovery::Code) continue;
        const u32 pc = img.text_off + (u32)w * 4, i = img.Word(pc);
        Tr t = Decode(i, pc);
        if (t.handled) { handled++; continue; }
        unhandled++;
        const u32 key = (i >> 28 == 15) ? (i & 0xFFF000F0u) : (i & 0x0FF000F0u);
        auto& e = sig[key];
        if (e.first++ == 0) e.second = {i, pc};
    }
    printf("translated: %zu of %zu discovered instructions (%.3f%%), unhandled %zu (%.3f%%)\n", handled,
           handled + unhandled, 100.0 * handled / (handled + unhandled), unhandled,
           100.0 * unhandled / (handled + unhandled));
    std::vector<std::pair<size_t, std::pair<u32, u32>>> top;
    for (auto& [k, v] : sig) top.push_back(v);
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t k = 0; k < top.size() && k < unh_limit; ++k)
        printf("UNH %zu %08X %08X\n", top[k].first, top[k].second.first, top[k].second.second);

    if (emit_dir.empty()) return 0;

    // Runtime sources are read from the directory given by --rt (default: cwd).
    auto slurp = [](const std::string& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    RuntimeSources rt{slurp(rt_dir + "/a32_runtime.h"), slurp(rt_dir + "/a32_host_runtime.c")};
    if (rt.runtime_h.empty() || rt.host_runtime_c.empty()) { fprintf(stderr, "runtime sources not found in %s\n", rt_dir.c_str()); return 1; }
    const std::string mod = mod_name.empty() ? "main" : mod_name;
    EmitStats es = EmitProject32(mod, img, d, emit_dir, rt, nfiles);
    printf("emitted module '%s': %zu blocks, %zu instructions (%zu unhandled) into %zu files under %s\n",
           mod.c_str(), es.blocks, es.instructions, es.unhandled, es.files, emit_dir.c_str());
    return 0;
}
