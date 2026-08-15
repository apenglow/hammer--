// Verifies the mip-selective VTF decode against real game files: for each
// material, decode at native resolution and at a thumbnail cap, then compare
// dimensions and average colour. A wrong mip offset decodes garbage, which shows
// up immediately as a large colour divergence.
#include "MaterialSystem.hpp"
#include "GameFileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace {

struct Average { double r{}, g{}, b{}; };

Average averageColor(const hammer::assets::Image& image)
{
    Average result;
    if (image.pixels.empty()) return result;
    for (const std::uint32_t pixel : image.pixels) {
        result.r += (pixel >> 16) & 255u;
        result.g += (pixel >> 8) & 255u;
        result.b += pixel & 255u;
    }
    const double count = static_cast<double>(image.pixels.size());
    result.r /= count; result.g /= count; result.b /= count;
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gameinfo.txt> <cap> [count]\n", argv[0]);
        return 2;
    }
    const int cap = std::atoi(argv[2]);
    const int limit = argc > 3 ? std::atoi(argv[3]) : 40;

    auto fileSystem = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError error;
    if (!fileSystem->configure(argv[1], &error)) {
        std::fprintf(stderr, "configure failed\n");
        return 1;
    }
    // Two systems: previewMaterial deliberately reuses a full-quality cache
    // entry when one exists, so priming the full cache first would mask the
    // downscale entirely (it did, on the first run of this check).
    auto materials = std::make_shared<hammer::assets::MaterialSystem>(fileSystem);
    auto previews = std::make_shared<hammer::assets::MaterialSystem>(fileSystem);
    const auto names = materials->materialNames();
    std::fprintf(stderr, "materials indexed: %zu\n", names.size());

    // Total resident pixel bytes across every Image a Material holds, which is
    // what actually accumulates in the cache.
    auto materialBytes = [](const hammer::assets::Material& m) {
        std::size_t total = 0;
        for (const hammer::assets::Image* image : {&m.image, &m.image2, &m.detailImage,
                &m.bumpImage, &m.lightWarpImage, &m.phongExponentImage,
                &m.selfIllumMaskImage, &m.waterNormalImage, &m.waterFlowImage}) {
            total += image->pixels.size() * sizeof(std::uint32_t);
        }
        for (const auto& face : m.envMapCube.faces)
            total += face.pixels.size() * sizeof(std::uint32_t);
        return total;
    };
    std::size_t fullBytes = 0, previewBytes = 0;
    int checked = 0, mismatches = 0, downscaled = 0;
    double worst = 0.0;
    std::string worstName;
    for (const std::string& name : names) {
        if (checked >= limit) break;
        const auto full = materials->material(name);
        if (!full || !full->image.valid()) continue;
        const auto preview = previews->previewMaterial(name, cap);
        if (!preview || !preview->image.valid()) continue;
        ++checked;
        fullBytes += materialBytes(*full);
        previewBytes += materialBytes(*preview);

        const int fullMax = std::max(full->image.width, full->image.height);
        const int previewMax = std::max(preview->image.width, preview->image.height);
        if (fullMax > cap) {
            if (previewMax >= fullMax) {
                std::printf("NOT DOWNSCALED %s: full=%dx%d preview=%dx%d\n", name.c_str(),
                            full->image.width, full->image.height,
                            preview->image.width, preview->image.height);
                ++mismatches;
                continue;
            }
            ++downscaled;
            // The chosen mip must still cover the requested cap.
            if (previewMax < cap) {
                std::printf("TOO SMALL %s: preview=%dx%d cap=%d\n", name.c_str(),
                            preview->image.width, preview->image.height, cap);
                ++mismatches;
                continue;
            }
        }

        const Average a = averageColor(full->image);
        const Average b = averageColor(preview->image);
        const double delta = (std::abs(a.r - b.r) + std::abs(a.g - b.g) +
                              std::abs(a.b - b.b)) / 3.0;
        if (delta > worst) { worst = delta; worstName = name; }
        // A correct smaller mip is the same image, so mean colour barely moves.
        // Garbage from a bad offset diverges wildly.
        if (delta > 12.0) {
            std::printf("COLOUR MISMATCH %s: full=(%.1f %.1f %.1f) preview=(%.1f %.1f %.1f) "
                        "delta=%.1f (%dx%d -> %dx%d)\n", name.c_str(),
                        a.r, a.g, a.b, b.r, b.g, b.b, delta,
                        full->image.width, full->image.height,
                        preview->image.width, preview->image.height);
            ++mismatches;
        }
    }
    const double fullMb = fullBytes / 1048576.0;
    const double previewMb = previewBytes / 1048576.0;
    std::printf("\nchecked=%d downscaled=%d mismatches=%d worstDelta=%.2f (%s)\n",
                checked, downscaled, mismatches, worst, worstName.c_str());
    std::printf("resident: full=%.1f MB preview=%.2f MB  ratio=%.0fx  perMaterial full=%.2f MB\n",
                fullMb, previewMb, previewBytes ? (double)fullBytes / previewBytes : 0.0,
                checked ? fullMb / checked : 0.0);
    std::printf("extrapolated to %zu materials: full=%.1f GB preview=%.0f MB\n",
                names.size(),
                checked ? fullMb / checked * names.size() / 1024.0 : 0.0,
                checked ? previewMb / checked * names.size() : 0.0);
    return mismatches ? 1 : 0;
}
