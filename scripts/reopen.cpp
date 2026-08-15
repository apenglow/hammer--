// Simulates closing and reopening the texture browser: repeated full preview
// sweeps at the same cap, reporting RSS and cache growth after each pass.
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
    const int count = argc > 2 ? std::atoi(argv[2]) : 300;
    const int passes = argc > 3 ? std::atoi(argv[3]) : 4;
    const int cap = argc > 4 ? std::atoi(argv[4]) : 64;

    auto fs = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError error;
    if (!fs->configure(argv[1], &error)) { std::fprintf(stderr, "configure failed\n"); return 1; }
    auto materials = std::make_shared<hammer::assets::MaterialSystem>(fs);
    const auto names = materials->materialNames();

    std::printf("baseline RSS = %ld KB (cap=%d, %d materials)\n", residentKb(), cap, count);
    for (int pass = 1; pass <= passes; ++pass) {
        int loaded = 0;
        // One "open": sweep every listed material, exactly as the browser does,
        // then drop all references, as closing the dialog does.
        for (const std::string& name : names) {
            if (loaded >= count) break;
            const auto preview = materials->previewMaterial(name, cap);
            if (preview && preview->image.valid()) ++loaded;
            // The detail pane loads the selected material at 256.
            if (loaded == 1) (void)materials->previewMaterial(name, 256);
        }
        std::printf("after open #%d = %ld KB\n", pass, residentKb());
    }
    std::printf("purge -> freed %.2f MB, RSS = %ld KB\n",
                materials->purgeUnusedMaterials() / 1048576.0, residentKb());
    return 0;
}
