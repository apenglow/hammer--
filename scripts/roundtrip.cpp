// mode=fresh : load every preview at 32 and stop (what a first open does)
// mode=trip  : load at 64, switch to 32, load at 32 (the round trip)
// Reports RSS plus the bytes actually resident in the cache, so retained
// allocator scratch can be told apart from real image data.
#include "MaterialSystem.hpp"
#include "GameFileSystem.hpp"
#include <malloc.h>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

static long residentKb()
{
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key)
        if (key == "VmRSS:") { long v = 0; status >> v; return v; }
    return 0;
}

int main(int argc, char** argv)
{
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    mallopt(M_TRIM_THRESHOLD, 4 * 1024 * 1024);
    const std::string mode = argc > 2 ? argv[2] : "fresh";
    const int count = argc > 3 ? std::atoi(argv[3]) : 1500;

    auto fs = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError error;
    if (!fs->configure(argv[1], &error)) { std::fprintf(stderr, "configure failed\n"); return 1; }
    auto materials = std::make_shared<hammer::assets::MaterialSystem>(fs);
    const auto names = materials->materialNames();
    const long baseline = residentKb();

    std::size_t residentImageBytes = 0;
    auto sweep = [&](int cap) {
        int loaded = 0;
        residentImageBytes = 0;
        for (const std::string& name : names) {
            if (loaded >= count) break;
            const auto preview = materials->previewMaterial(name, cap);
            if (!preview || !preview->image.valid()) continue;
            ++loaded;
            residentImageBytes += preview->image.pixels.size() * sizeof(std::uint32_t);
        }
        return loaded;
    };

    int loaded = 0;
    if (mode == "fresh") {
        loaded = sweep(32);
    } else {
        sweep(64);
        materials->adjustPreviewCache(32);
        loaded = sweep(32);
    }

    std::printf("mode=%-5s loaded=%d  RSS=%ld KB (+%ld KB over baseline)  "
                "base images resident=%.2f MB\n",
                mode.c_str(), loaded, residentKb(), residentKb() - baseline,
                residentImageBytes / 1048576.0);
    malloc_trim(0);
    std::printf("            after malloc_trim RSS=%ld KB\n", residentKb());
    return 0;
}
