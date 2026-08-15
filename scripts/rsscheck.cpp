// Reproduces "load everything at 256, switch to 32, memory does not drop".
// Distinguishes two very different failures:
//   (a) the cache is not actually releasing entries  -> logic bug
//   (b) the cache releases but process RSS stays high -> allocator retention
#include "MaterialSystem.hpp"
#include "GameFileSystem.hpp"

#include <malloc.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

long residentKb()
{
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") { long value = 0; status >> value; return value; }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <gameinfo.txt> [count]\n", argv[0]);
        return 2;
    }
    const int count = argc > 2 ? std::atoi(argv[2]) : 300;
    if (!getenv("RSSCHECK_NO_MALLOPT")) {
        mallopt(M_MMAP_THRESHOLD, 128 * 1024);
        mallopt(M_TRIM_THRESHOLD, 4 * 1024 * 1024);
    }

    auto fileSystem = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError error;
    if (!fileSystem->configure(argv[1], &error)) {
        std::fprintf(stderr, "configure failed\n");
        return 1;
    }
    auto materials = std::make_shared<hammer::assets::MaterialSystem>(fileSystem);
    const auto names = materials->materialNames();

    const long baseline = residentKb();
    std::printf("baseline RSS        = %ld KB\n", baseline);

    // Populate exactly as the browser does at 256 px, holding no references
    // afterwards (the browser keeps only QPixmap icons).
    int loaded = 0;
    for (const std::string& name : names) {
        if (loaded >= count) break;
        const auto preview = materials->previewMaterial(name, 256);
        if (preview && preview->image.valid()) ++loaded;
    }
    const long after256 = residentKb();
    std::printf("after %4d @256      = %ld KB  (+%ld KB)\n",
                loaded, after256, after256 - baseline);

    // Now the user's action: switch the browser to 32 px.
    const std::size_t freed = materials->adjustPreviewCache(32);
    const long after32 = residentKb();
    std::printf("adjustPreviewCache(32) reported freed = %.2f MB\n", freed / 1048576.0);
    std::printf("after switch to 32   = %ld KB  (%+ld KB)\n",
                after32, after32 - after256);

    // Ask glibc to return free arena pages to the OS. If RSS only drops here,
    // the cache logic was correct and the retention was purely the allocator.
    const bool explicitTrim = getenv("RSSCHECK_TRIM") != nullptr;
    if (explicitTrim) malloc_trim(0);
    const long afterTrim = residentKb();
    std::printf("after malloc_trim    = %ld KB  (%+ld KB)\n",
                afterTrim, afterTrim - after32);

    // Confirm the entries really are small now.
    std::size_t residentAt32 = 0;
    int sampled = 0;
    for (const std::string& name : names) {
        if (sampled >= loaded) break;
        const auto preview = materials->previewMaterial(name, 32);
        if (!preview || !preview->image.valid()) continue;
        ++sampled;
        residentAt32 += preview->image.pixels.size() * sizeof(std::uint32_t);
        if (sampled <= 3) {
            std::printf("  sample %-40s %dx%d\n", name.c_str(),
                        preview->image.width, preview->image.height);
        }
    }
    std::printf("base-image bytes across %d previews at 32 px = %.2f MB\n",
                sampled, residentAt32 / 1048576.0);
    std::printf("\nverdict: %s\n",
                (after32 - after256) < -1024 ? "RSS dropped at switch"
                : (afterTrim - after32) < -1024 ? "RSS only dropped after malloc_trim (allocator retention)"
                : "RSS did NOT drop (cache is still holding memory)");
    return 0;
}
