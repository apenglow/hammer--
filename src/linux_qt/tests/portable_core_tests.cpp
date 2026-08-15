#include "blockarray.h"
#include "viewersettings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

extern double I_FloatTime();
extern void I_BeginTime();
extern double I_EndTime();
extern float RandomNoise2D(int x, int y);
extern float PerlinNoise2D(float x, float y, float rockiness);

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main()
{
    BlockArray<int, 8, 4> values;
    values[19] = 73;
    require(values.GetCount() == 20, "BlockArray grows on indexed access");
    require(values[19] == 73, "BlockArray retains values across blocks");

    bool negativeRejected = false;
    try {
        (void)values[-1];
    } catch (const std::out_of_range&) {
        negativeRejected = true;
    }
    require(negativeRejected, "BlockArray rejects negative indices on Linux");

    InitViewerSettings();
    require(g_viewerSettings.renderMode == RM_TEXTURED, "viewer settings default to textured rendering");
    require(g_viewerSettings.trans[2] == 50.0f, "viewer translation default stays within the array");

    const std::filesystem::path settingsPath =
        std::filesystem::temp_directory_path() / "hammer-viewer-settings-test.bin";
    require(SaveViewerSettings(settingsPath.c_str()) == 1, "viewer settings save");
    g_viewerSettings.renderMode = RM_WIREFRAME;
    require(LoadViewerSettings(settingsPath.c_str()) == 1, "viewer settings load");
    require(g_viewerSettings.renderMode == RM_TEXTURED, "viewer settings round trip");
    std::error_code removeError;
    std::filesystem::remove(settingsPath, removeError);

    const float first = RandomNoise2D(12, 34);
    require(first == RandomNoise2D(12, 34), "noise is deterministic");
    require(std::isfinite(PerlinNoise2D(1.25f, 9.5f, 0.5f)), "Perlin noise is finite");

    const double before = I_FloatTime();
    I_BeginTime();
    const double elapsed = I_EndTime();
    require(before >= 0.0, "portable timer starts at zero or later");
    require(elapsed >= 0.0, "portable elapsed timer is nonnegative");

    std::cout << "portable original-source tests passed\n";
    return EXIT_SUCCESS;
}
