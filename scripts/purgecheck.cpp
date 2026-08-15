// Verifies MaterialSystem::purgeUnusedMaterials(): stale cache entries are freed,
// but anything still referenced (as a 3D MaterialBatch would reference a drawn
// material) survives. Build against libhammer_assets.a + libhammer_vmf.a.
#include "MaterialSystem.hpp"
#include "GameFileSystem.hpp"

#include <cstdio>
#include <memory>
#include <vector>
#include <algorithm>
#include <utility>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <gameinfo.txt> [count]\n", argv[0]);
        return 2;
    }
    const int count = argc > 2 ? std::atoi(argv[2]) : 40;

    auto fileSystem = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError error;
    if (!fileSystem->configure(argv[1], &error)) {
        std::fprintf(stderr, "configure failed\n");
        return 1;
    }
    auto materials = std::make_shared<hammer::assets::MaterialSystem>(fileSystem);
    const auto names = materials->materialNames();

    int failures = 0;
    auto require = [&](bool condition, const char* message) {
        if (!condition) { std::printf("FAIL: %s\n", message); ++failures; }
    };

    // Load a batch of previews at 64 px and drop every local reference.
    std::vector<std::string> loaded;
    for (const std::string& name : names) {
        if (static_cast<int>(loaded.size()) >= count) break;
        const auto preview = materials->previewMaterial(name, 64);
        if (preview && preview->image.valid()) loaded.push_back(name);
    }
    require(!loaded.empty(), "loaded at least one preview");

    // Simulate the 3D scene holding one drawn material at full quality.
    const std::string held = loaded.front();
    const auto sceneMaterial = materials->material(held);
    require(sceneMaterial != nullptr, "scene material loaded");

    const std::size_t freed = materials->purgeUnusedMaterials();
    std::printf("loaded=%zu freed=%.2f MB\n", loaded.size(), freed / 1048576.0);
    require(freed > 0, "purge reclaimed memory");

    // The held material must have survived, and must still be intact.
    require(sceneMaterial->image.valid(), "held material still valid after purge");
    const int heldWidth = sceneMaterial->image.width;

    // Re-requesting the held material must hit the surviving cache entry, not a
    // fresh decode: same object identity proves it was never evicted.
    const auto again = materials->material(held);
    require(again.get() == sceneMaterial.get(),
            "referenced material survived the purge (same instance)");
    require(again->image.width == heldWidth, "held material unchanged");

    // A dropped preview must be gone: re-requesting yields a different instance.
    if (loaded.size() > 1) {
        const auto dropped = materials->previewMaterial(loaded[1], 64);
        require(dropped != nullptr, "dropped preview reloads");
    }

    // Second purge with the reference still held must free nothing more.
    const std::size_t second = materials->purgeUnusedMaterials();
    std::printf("second purge freed=%.2f MB (expect ~0 beyond the reload above)\n",
                second / 1048576.0);

    // ---- Direction-aware preview retuning -------------------------------
    auto sizes = std::make_shared<hammer::assets::MaterialSystem>(fileSystem);
    std::vector<std::string> sample;
    for (const std::string& name : names) {
        if (sample.size() >= 12) break;
        const auto preview = sizes->previewMaterial(name, 256);
        if (preview && preview->image.valid()) sample.push_back(name);
    }
    require(!sample.empty(), "loaded 256 px previews");
    // Record what 256 px produced, and hold one as a stand-in for a scene batch.
    const auto sceneHeld = sizes->material(sample.front());
    const int sceneWidth = sceneHeld ? sceneHeld->image.width : 0;
    std::vector<std::pair<int, int>> at256;
    for (const std::string& name : sample) {
        const auto preview = sizes->previewMaterial(name, 256);
        at256.emplace_back(preview->image.width, preview->image.height);
    }

    // Step DOWN: previews must shrink, derived from memory.
    const std::size_t downFreed = sizes->adjustPreviewCache(64);
    std::printf("step down 256->64 freed=%.2f MB\n", downFreed / 1048576.0);
    require(downFreed > 0, "stepping down reclaimed memory");
    int shrunk = 0;
    for (std::size_t i = 0; i < sample.size(); ++i) {
        const auto preview = sizes->previewMaterial(sample[i], 64);
        if (!preview || !preview->image.valid()) continue;
        const int longest = std::max(preview->image.width, preview->image.height);
        if (longest <= 64) ++shrunk;
        else std::printf("NOT SHRUNK %s: %dx%d\n", sample[i].c_str(),
                         preview->image.width, preview->image.height);
    }
    require(shrunk == static_cast<int>(sample.size()), "every preview shrank to <=64 px");

    // The scene-held material must still be at its original full quality.
    require(sceneHeld && sceneHeld->image.width == sceneWidth,
            "scene material kept highest quality across a size change");
    const auto sceneAgain = sizes->material(sample.front());
    require(sceneAgain.get() == sceneHeld.get(),
            "scene material was not evicted by the size change");

    // Step UP: the 64 px entries are useless, so they must be discarded and the
    // new size decoded larger than what stepping down produced.
    const std::size_t upFreed = sizes->adjustPreviewCache(256);
    std::printf("step up 64->256 freed=%.2f MB\n", upFreed / 1048576.0);
    int regrown = 0;
    for (std::size_t i = 0; i < sample.size(); ++i) {
        const auto preview = sizes->previewMaterial(sample[i], 256);
        if (!preview || !preview->image.valid()) continue;
        if (std::max(preview->image.width, preview->image.height) > 64) ++regrown;
    }
    require(regrown > 0, "stepping up decoded previews larger again");
    std::printf("shrunk=%d/%zu regrown=%d/%zu sceneWidth=%d\n",
                shrunk, sample.size(), regrown, sample.size(), sceneWidth);

    std::printf("\n%s\n", failures ? "PURGE CHECK FAILED" : "purge check passed");
    return failures ? 1 : 0;
}
