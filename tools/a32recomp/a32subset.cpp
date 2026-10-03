// a32subset: prepare a whole-function differential test from a real module.
//
//   a32subset <module.nso> <base_hex> <count> <seed> <outdir> [--rt dir] [--max-blocks N]
//
// Picks `count` function entries (discovered prologues), collects every block
// reachable from them through direct branches and fall-through, and emits just
// those blocks as a project (same emitter the exporter uses). Also writes:
//   image.bin      the module image with R_ARM_RELATIVE relocations applied for `base`
//   functions.txt  entry offsets under test
#include "a32emit.h"
#include <cstdio>
#include <deque>
#include <random>

using namespace a32recomp;

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: a32subset module.nso base count seed outdir [--rt dir] [--max-blocks N]\n");
        return 2;
    }
    const u32 base = (u32)strtoul(argv[2], nullptr, 16);
    const unsigned count = (unsigned)atoi(argv[3]);
    const unsigned seed = (unsigned)atoi(argv[4]);
    const std::string out = argv[5];
    std::string rt_dir = ".";
    size_t max_blocks = 60000;
    for (int k = 6; k < argc; ++k) {
        if (std::string(argv[k]) == "--rt" && k + 1 < argc) rt_dir = argv[++k];
        else if (std::string(argv[k]) == "--max-blocks" && k + 1 < argc) max_blocks = (size_t)atol(argv[++k]);
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<u8> file((std::istreambuf_iterator<char>(f)), {});
    NsoImage img;
    if (!LoadNso(file, img)) { fprintf(stderr, "bad NSO\n"); return 1; }
    Discovery d = Discover(img);

    // Candidate entries: block starts that begin with push {..., lr}.
    std::vector<u32> cands;
    for (u32 a : d.block_starts)
        if ((img.Word(a) & 0xFFFF4000u) == 0xE92D4000u) cands.push_back(a);
    std::mt19937 rng(seed);
    std::shuffle(cands.begin(), cands.end(), rng);

    // Collect reachable blocks per function until the budget runs out.
    std::set<u32> subset;
    std::vector<u32> chosen;
    auto code = [&](u32 a) { return img.InText(a) && d.kind[(a - img.text_off) / 4] == Discovery::Code; };
    for (u32 entry : cands) {
        if (chosen.size() >= count) break;
        std::set<u32> local;
        std::deque<u32> work{entry};
        bool too_big = false;
        while (!work.empty()) {
            u32 a = work.front();
            work.pop_front();
            if (!code(a) || local.count(a)) continue;
            local.insert(a);
            if (local.size() > 3000) { too_big = true; break; }
            // walk the block the way the emitter will (stop at the next known start)
            for (u32 pc = a;; pc += 4) {
                if (!code(pc)) break;
                if (pc != a && d.block_starts.count(pc)) { work.push_back(pc); break; }
                const u32 i = img.Word(pc);
                if ((i & 0x0E000000u) == 0x0A000000u && (i >> 28) != 15)
                    work.push_back((u32)(pc + 8 + ((s32)(i << 8) >> 6)));
                Tr t = Decode(i, pc);
                if (!t.handled) break;
                if (t.terminates) {
                    if ((i >> 28) != 14 || (i & 0x0F000000u) == 0x0B000000u || (i & 0x0FFFFFF0u) == 0x012FFF30u)
                        work.push_back(pc + 4);
                    break;
                }
            }
        }
        if (too_big || subset.size() + local.size() > max_blocks) continue;
        subset.insert(local.begin(), local.end());
        chosen.push_back(entry);
    }

    Discovery sub = d;
    sub.block_starts = subset;
    auto slurp = [](const std::string& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    RuntimeSources rt{slurp(rt_dir + "/a32_runtime.h"), slurp(rt_dir + "/a32_host_runtime.c")};
    EmitStats es = EmitProject32("main", img, sub, out, rt, 16);

    // Relocated image: R_ARM_RELATIVE adds the load base to the stored word.
    std::vector<u8> mem = img.mem;
    const u32 mod0 = img.Word(img.text_off + 4);
    size_t relocs = 0;
    if (mod0 + 8 <= mem.size() && memcmp(&mem[mod0], "MOD0", 4) == 0) {
        const u32 dyn = mod0 + img.Word(mod0 + 4);
        u32 rel = 0, relsz = 0;
        for (u32 p = dyn; p + 8 <= mem.size(); p += 8) {
            const u32 tag = img.Word(p), val = img.Word(p + 4);
            if (!tag) break;
            if (tag == 17) rel = val;
            if (tag == 18) relsz = val;
        }
        for (u32 p = rel; p + 8 <= rel + relsz; p += 8) {
            const u32 off = img.Word(p), info = img.Word(p + 4);
            if ((info & 0xFF) == 23 && off + 4 <= mem.size()) {
                u32 v;
                memcpy(&v, &mem[off], 4);
                v += base;
                memcpy(&mem[off], &v, 4);
                relocs++;
            }
        }
    }
    {
        std::ofstream o(out + "/image.bin", std::ios::binary);
        o.write((const char*)mem.data(), (std::streamsize)mem.size());
        std::ofstream fl(out + "/functions.txt");
        for (u32 e : chosen) fl << F("%X\n", e);
    }
    printf("functions %zu, blocks %zu, instructions %zu (unhandled %zu), relocations %zu, image %zu bytes\n",
           chosen.size(), es.blocks, es.instructions, es.unhandled, relocs, mem.size());
    return 0;
}
