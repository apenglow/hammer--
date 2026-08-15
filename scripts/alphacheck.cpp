// Reports why a material does or does not transmit light in the ray-traced
// preview.
//
// A shadow ray only passes through a surface when the material declares
// coverage ($alphatest or $translucent/$alpha) AND the decoded base texture
// actually carries the alpha that coverage is read from. This dumps both halves
// for named materials, or scans every indexed material for the mismatch that
// makes a fence or a window cast a solid shadow: coverage declared but the base
// image is fully opaque, or alpha present but no coverage keyword.
//
// usage: alphacheck <gameinfo.txt> [material ...]
//        alphacheck <gameinfo.txt> --scan [limit]
#include "MaterialSystem.hpp"
#include "GameFileSystem.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

struct AlphaProfile
{
    bool valid{false};
    int minimum{255};
    int maximum{0};
    double mean{0.0};
    // Fraction of texels that an $alphatest cut at `reference` would discard.
    double cutFraction{0.0};
};

AlphaProfile profileAlpha(const hammer::assets::Image& image, float reference)
{
    AlphaProfile profile;
    if (!image.valid() || image.pixels.empty()) return profile;
    profile.valid = true;
    const int cut = static_cast<int>(reference * 255.0f);
    double total = 0.0;
    std::size_t discarded = 0;
    for (const std::uint32_t pixel : image.pixels) {
        const int alpha = static_cast<int>((pixel >> 24) & 255u);
        profile.minimum = alpha < profile.minimum ? alpha : profile.minimum;
        profile.maximum = alpha > profile.maximum ? alpha : profile.maximum;
        total += alpha;
        if (alpha < cut) ++discarded;
    }
    profile.mean = total / static_cast<double>(image.pixels.size());
    profile.cutFraction =
        static_cast<double>(discarded) / static_cast<double>(image.pixels.size());
    return profile;
}

void report(const hammer::assets::Material& material)
{
    const AlphaProfile alpha = profileAlpha(material.image, material.alphaTestReference);
    std::printf("%-52s shader=%-22s\n", material.name.c_str(), material.shader.c_str());
    std::printf("    $alphatest=%d ref=%.3f  $translucent=%d $alpha=%.3f\n",
                material.alphaTest ? 1 : 0, material.alphaTestReference,
                material.translucent ? 1 : 0, material.alpha);
    if (!alpha.valid) {
        std::printf("    base image: MISSING - nothing to read coverage from\n\n");
        return;
    }
    std::printf("    base alpha: min=%d max=%d mean=%.1f  alphatest would cut %.1f%%\n",
                alpha.minimum, alpha.maximum, alpha.mean, alpha.cutFraction * 100.0);

    const bool declaresCoverage = material.alphaTest || material.translucent;
    const bool hasAlpha = alpha.minimum < 250;
    if (!declaresCoverage && hasAlpha) {
        std::printf("    -> OPAQUE to light: the texture has alpha but the VMT declares "
                    "neither $alphatest nor $translucent, so Source (and the preview) "
                    "treat that alpha as a mask, not as coverage.\n");
    } else if (declaresCoverage && !hasAlpha) {
        std::printf("    -> OPAQUE to light: coverage is declared but every texel is "
                    "opaque, so there is nothing for light to pass through.\n");
    } else if (declaresCoverage && hasAlpha) {
        std::printf("    -> transmits light.\n");
    } else {
        std::printf("    -> opaque, as authored.\n");
    }
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <gameinfo.txt> [material ...]\n", argv[0]);
        std::fprintf(stderr, "       %s <gameinfo.txt> --scan [limit]\n", argv[0]);
        return 2;
    }

    auto fileSystem = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError error;
    if (!fileSystem->configure(argv[1], &error)) {
        std::fprintf(stderr, "configure failed: %s\n", error.message.c_str());
        return 1;
    }
    auto materials = std::make_shared<hammer::assets::MaterialSystem>(fileSystem);

    if (argc > 2 && std::strcmp(argv[2], "--scan") != 0) {
        for (int i = 2; i < argc; ++i) {
            const auto material = materials->material(argv[i]);
            if (!material) {
                std::printf("%-52s NOT FOUND\n\n", argv[i]);
                continue;
            }
            report(*material);
        }
        return 0;
    }

    const int limit = argc > 3 ? std::atoi(argv[3]) : 40;
    const auto names = materials->materialNames();
    std::fprintf(stderr, "materials indexed: %zu\n", names.size());
    int reported = 0;
    for (const std::string& name : names) {
        if (reported >= limit) break;
        const auto material = materials->material(name);
        if (!material) continue;
        const AlphaProfile alpha = profileAlpha(material->image, material->alphaTestReference);
        if (!alpha.valid) continue;
        const bool declaresCoverage = material->alphaTest || material->translucent;
        const bool hasAlpha = alpha.minimum < 250;
        // Only the surprising combinations are worth printing.
        if (declaresCoverage == hasAlpha) continue;
        report(*material);
        ++reported;
    }
    return 0;
}
