#include "VulkanRayTracedViewport.hpp"

#include "EnvCubemap.hpp"
#include "MapViewWidget.hpp"
#include "RayTracingScene.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QApplication>
#include <QTimer>

#include "PreviewRenderGate.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t ControlPhong = 1u << 0;
constexpr std::uint32_t ControlSpecular = 1u << 1;
constexpr std::uint32_t ControlBump = 1u << 2;
constexpr std::uint32_t ControlLightWarp = 1u << 3;
constexpr std::uint32_t ControlSelfIllum = 1u << 4;
constexpr std::uint32_t ControlRimLight = 1u << 5;
constexpr std::uint32_t ControlDenoise = 1u << 6;
constexpr std::uint32_t ControlInteractive = 1u << 7;

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    if (alignment <= 1) return value;
    return (value + alignment - 1) & ~(alignment - 1);
}

QString vkResultText(VkResult result)
{
    switch (result) {
    case VK_SUCCESS: return QStringLiteral("success");
    case VK_ERROR_OUT_OF_HOST_MEMORY: return QStringLiteral("out of host memory");
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return QStringLiteral("out of device memory");
    case VK_ERROR_INITIALIZATION_FAILED: return QStringLiteral("initialization failed");
    case VK_ERROR_DEVICE_LOST: return QStringLiteral("device lost");
    case VK_ERROR_FEATURE_NOT_PRESENT: return QStringLiteral("feature not present");
    case VK_ERROR_EXTENSION_NOT_PRESENT: return QStringLiteral("extension not present");
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return QStringLiteral("format not supported");
    default: return QStringLiteral("Vulkan error %1").arg(static_cast<int>(result));
    }
}

struct alignas(16) CameraGpu
{
    std::array<float, 4> cameraPosition{};
    std::array<float, 4> cameraForwardTanHalfFov{};
    std::array<float, 4> cameraRightAspect{};
    std::array<float, 4> cameraUpNear{};
    std::array<float, 4> sunDirectionIntensity{};
    std::array<float, 4> sunColorTime{};
    std::array<float, 4> effectIntensities{};
    std::array<float, 4> backgroundColor{};
    std::array<std::uint32_t, 4> renderControls{};
    std::array<std::array<float, 4>, 6> skyRects{};
    // x = Source tonemap/exposure scale, y = bloom scale, z = default
    // uncorrected color weight, w = reserved.
    std::array<float, 4> toneMapControls{1.0f, 1.0f, 1.0f, 0.0f};
    std::array<float, 4> colorCorrectionWeights{};
    std::array<std::uint32_t, 4> colorCorrectionIndices{};
    // Radiosity patch grid: xy = patch atlas size, z = 1 when the bounce solve
    // has run and the patch buffers hold usable light, w = reserved.
    std::array<std::uint32_t, 4> radiosityControls{};
};
static_assert(sizeof(CameraGpu) == 304);

} // namespace

class VulkanRayTracedViewport::Renderer
{
public:
    Renderer() = default;
    ~Renderer() { shutdown(); }

    bool initialize(QString& error)
    {
        if (device_) return true;

        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Hammer++ Ray-Traced Preview";
        application.applicationVersion = VK_MAKE_VERSION(0, 15, 26);
        application.pEngineName = "Hammer++ Vulkan Preview";
        application.engineVersion = VK_MAKE_VERSION(0, 15, 26);
        application.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &application;
        VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create Vulkan 1.2 instance: %1")
                        .arg(vkResultText(result));
            return false;
        }

        std::uint32_t physicalCount = 0;
        vkEnumeratePhysicalDevices(instance_, &physicalCount, nullptr);
        if (!physicalCount) {
            error = QStringLiteral("No Vulkan physical devices were found");
            return false;
        }
        std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
        vkEnumeratePhysicalDevices(instance_, &physicalCount, physicalDevices.data());

        // Prefer a real discrete GPU. Enumeration order is not a performance
        // guarantee and may place an integrated or software Vulkan device first.
        int bestScore = std::numeric_limits<int>::min();
        VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
        std::uint32_t bestQueueFamily = 0;
        VkPhysicalDeviceProperties bestProperties{};
        VkDeviceSize bestScratchAlignment = 256;
        for (VkPhysicalDevice candidate : physicalDevices) {
            VkPhysicalDeviceProperties candidateProperties{};
            std::uint32_t candidateQueueFamily = 0;
            VkDeviceSize candidateScratchAlignment = 256;
            const int score = scorePhysicalDevice(candidate, candidateProperties,
                                                  candidateQueueFamily,
                                                  candidateScratchAlignment);
            if (score <= bestScore) continue;
            bestScore = score;
            bestDevice = candidate;
            bestQueueFamily = candidateQueueFamily;
            bestProperties = candidateProperties;
            bestScratchAlignment = candidateScratchAlignment;
        }
        if (bestDevice) {
            physicalDevice_ = bestDevice;
            queueFamily_ = bestQueueFamily;
            deviceProperties_ = bestProperties;
            scratchAlignment_ = bestScratchAlignment;
        }
        if (!physicalDevice_) {
            error = QStringLiteral(
                "No Vulkan device exposes VK_KHR_acceleration_structure and VK_KHR_ray_query");
            return false;
        }

        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddress{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        bufferAddress.bufferDeviceAddress = VK_TRUE;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        accelerationFeatures.accelerationStructure = VK_TRUE;
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        rayQueryFeatures.rayQuery = VK_TRUE;
        bufferAddress.pNext = &accelerationFeatures;
        accelerationFeatures.pNext = &rayQueryFeatures;

        const std::array<const char*, 3> extensions{
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        };
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &bufferAddress;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        deviceInfo.ppEnabledExtensionNames = extensions.data();
        result = vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create Vulkan ray-query device: %1")
                        .arg(vkResultText(result));
            return false;
        }
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

        createAccelerationStructure_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
            vkGetDeviceProcAddr(device_, "vkCreateAccelerationStructureKHR"));
        destroyAccelerationStructure_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
            vkGetDeviceProcAddr(device_, "vkDestroyAccelerationStructureKHR"));
        getAccelerationStructureBuildSizes_ =
            reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
                vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
        commandBuildAccelerationStructures_ =
            reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
                vkGetDeviceProcAddr(device_, "vkCmdBuildAccelerationStructuresKHR"));
        getAccelerationStructureDeviceAddress_ =
            reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
                vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureDeviceAddressKHR"));
        if (!createAccelerationStructure_ || !destroyAccelerationStructure_ ||
            !getAccelerationStructureBuildSizes_ || !commandBuildAccelerationStructures_ ||
            !getAccelerationStructureDeviceAddress_) {
            error = QStringLiteral("The Vulkan driver omitted required acceleration-structure entry points");
            return false;
        }

        VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        commandPoolInfo.queueFamilyIndex = queueFamily_;
        result = vkCreateCommandPool(device_, &commandPoolInfo, nullptr, &commandPool_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create Vulkan command pool: %1")
                        .arg(vkResultText(result));
            return false;
        }
        VkCommandBufferAllocateInfo commandBufferInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandBufferInfo.commandPool = commandPool_;
        commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandBufferInfo.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device_, &commandBufferInfo, &commandBuffer_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not allocate Vulkan command buffer: %1")
                        .arg(vkResultText(result));
            return false;
        }
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(device_, &fenceInfo, nullptr, &fence_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create Vulkan fence: %1").arg(vkResultText(result));
            return false;
        }

        if (!createPipeline(error)) return false;
        if (!createRadiosityPipelines(error)) return false;
        hardwareRayTracing_ = true;
        return true;
    }

    QString description() const
    {
        if (!physicalDevice_) return QStringLiteral("Vulkan ray tracing unavailable");
        return QStringLiteral("Vulkan ray-query preview — %1%2")
            .arg(QString::fromUtf8(deviceProperties_.deviceName),
                 hardwareRayTracing_ ? QStringLiteral(" (hardware RT)") : QString{});
    }

    bool hardwareRayTracingAvailable() const { return hardwareRayTracing_; }
    bool hasAnimatedContent() const { return scene_.hasAnimatedContent; }
    bool hasPendingDeferredRefresh() const {
        return spriteBillboardRefreshPending_ || animationRefreshPending_;
    }
    void requestAnimationRefresh() { animationRefreshPending_ = true; }

    bool render(MapViewWidget& owner, int width, int height, bool rebuildScene,
                QImage& frame, QString& error)
    {
        if (!initialize(error)) return false;
        if (width <= 0 || height <= 0) return false;

        const auto currentForward = hammer::camera::forwardVector(owner.cameraState_);
        const std::array<float, 6> cameraSignature{
            static_cast<float>(owner.cameraState_.position.x),
            static_cast<float>(owner.cameraState_.position.y),
            static_cast<float>(owner.cameraState_.position.z),
            static_cast<float>(currentForward.x),
            static_cast<float>(currentForward.y),
            static_cast<float>(currentForward.z)};
        bool cameraChanged = !hasCameraSignature_;
        if (!cameraChanged) {
            for (std::size_t i = 0; i < cameraSignature.size(); ++i) {
                if (std::abs(cameraSignature[i] - lastCameraSignature_[i]) > 0.0001f) {
                    cameraChanged = true;
                    break;
                }
            }
        }
        if (cameraChanged) {
            historyFrame_ = QImage{};
            lastCameraSignature_ = cameraSignature;
            hasCameraSignature_ = true;
            cameraStableTimer_.restart();
            // Keep the last billboard orientation while the camera is moving.
            // Once motion settles, refresh sprite vertices once via the cheap
            // host-visible upload + BLAS refit path.
            if (scene_.hasCameraFacingSprites) spriteBillboardRefreshPending_ = true;
        }
        const bool resetToneMapping = rebuildScene || !scene_.valid();
        const bool interactive = cameraChanged ||
            (cameraStableTimer_.isValid() && cameraStableTimer_.elapsed() < 140);

        const bool refreshSprites = !interactive && spriteBillboardRefreshPending_;
        const bool refreshAnimation = !interactive && animationRefreshPending_ &&
                                      scene_.hasAnimatedContent;
        // Animated VTF frames change atlas contents, so they still require a full
        // material/atlas refresh. Animated prop vertices and sprite billboards do not.
        if (refreshAnimation && scene_.hasAnimatedMaterialContent) rebuildScene = true;

        if (rebuildScene || !scene_.valid()) {
            historyFrame_ = QImage{};
            hammer::render::RayTracingBuildOptions options;
            options.hiddenToolTextures = owner.hiddenToolTextures_;
            options.displacementSolidMask = owner.displacementSolidMaskEnabled_;
            options.camera = owner.cameraState_;
            options.animationSeconds = animationTimer_.isValid()
                ? animationTimer_.elapsed() / 1000.0 : 0.0;
            options.maximumAtlasSize = static_cast<int>(std::min<std::uint32_t>(
                8192u, deviceProperties_.limits.maxImageDimension2D));
            options.maximumAtlasLayers = static_cast<int>(
                deviceProperties_.limits.maxImageArrayLayers);
            options.hdrLighting = owner.hdrEnabled_;
            options.detailProps = owner.detailPropsVisible_;
            if (!animationTimer_.isValid()) animationTimer_.start();
            hammer::render::RayTracingSceneBuilder builder(owner.materials_, owner.studioModels_);
            scene_ = owner.scene_ ? builder.build(*owner.scene_, options)
                                  : hammer::render::RayTracingScene{};
            if (!scene_.valid()) {
                error = QString::fromStdString(scene_.error.empty()
                    ? std::string("The map contains no ray-traceable geometry") : scene_.error);
                return false;
            }
            if (!uploadScene(error)) return false;
            if (resetToneMapping || !toneMapTimer_.isValid()) {
                const float initialScale = std::clamp(static_cast<float>(scene_.toneMap.scale), 0.001f, 16.0f);
                currentToneScale_ = initialScale;
                toneMapTimer_.restart();
            }
            spriteBillboardRefreshPending_ = false;
            animationRefreshPending_ = false;
        } else if ((refreshSprites || refreshAnimation) && owner.scene_) {
            hammer::render::RayTracingBuildOptions options;
            options.hiddenToolTextures = owner.hiddenToolTextures_;
            options.displacementSolidMask = owner.displacementSolidMaskEnabled_;
            options.camera = owner.cameraState_;
            options.animationSeconds = animationTimer_.isValid()
                ? animationTimer_.elapsed() / 1000.0 : 0.0;
            hammer::render::RayTracingSceneBuilder builder(owner.materials_, owner.studioModels_);
            bool updated = true;
            if (refreshAnimation)
                updated = builder.updateDynamicGeometry(*owner.scene_, options, scene_);
            else if (refreshSprites)
                updated = builder.updateSpriteGeometry(*owner.scene_, options, scene_);
            if (!updated || !uploadDynamicVertices(error)) {
                error = QStringLiteral("Could not refresh dynamic RT geometry");
                return false;
            }
            historyFrame_ = QImage{};
            if (refreshSprites) spriteBillboardRefreshPending_ = false;
            if (refreshAnimation) animationRefreshPending_ = false;
        }
        // Performance-balanced RT resolution. Native settled rendering already
        // looks substantially sharper than the old 50% path, while avoiding the
        // 1.56x pixel cost of v0.15.14 supersampling.
        const int renderWidth = interactive
            ? std::max(1, (width + 1) / 2)
            : width;
        const int renderHeight = interactive
            ? std::max(1, (height + 1) / 2)
            : height;
        if (outputWidth_ != renderWidth || outputHeight_ != renderHeight) {
            if (!createOutput(renderWidth, renderHeight, error)) return false;
        }
        if (!descriptorSet_ && !updateDescriptors(error)) return false;
        updateCamera(owner, renderWidth, renderHeight, interactive);

        if (!dispatchFrame(renderWidth, renderHeight, frame, error)) return false;

        // Apply a small edge-aware cross filter to stable frames, then blend
        // against the previous stable result. Camera motion and scene rebuilds
        // reset history so the denoiser cannot smear geometry across views.
        if (!interactive) {
            edgeAwareDenoise(frame);
            if (!historyFrame_.isNull() && historyFrame_.size() == frame.size())
                temporalAccumulate(frame, historyFrame_, 0.78f);
            historyFrame_ = frame.copy();
        } else {
            historyFrame_ = QImage{};
        }
        ++frameIndex_;
        return true;
    }

    static int rgbaChannelDistance(const uchar* a, const uchar* b)
    {
        return std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0])) +
               std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1])) +
               std::abs(static_cast<int>(a[2]) - static_cast<int>(b[2]));
    }

    static void edgeAwareDenoise(QImage& image)
    {
        if (image.width() < 3 || image.height() < 3 ||
            image.format() != QImage::Format_RGBA8888) return;
        const QImage source = image.copy();
        constexpr int edgeThreshold = 72;
        for (int y = 1; y + 1 < image.height(); ++y) {
            uchar* destination = image.scanLine(y);
            const uchar* centerRow = source.constScanLine(y);
            const uchar* upperRow = source.constScanLine(y - 1);
            const uchar* lowerRow = source.constScanLine(y + 1);
            for (int x = 1; x + 1 < image.width(); ++x) {
                const uchar* center = centerRow + x * 4;
                const uchar* samples[4]{center - 4, center + 4,
                                        upperRow + x * 4, lowerRow + x * 4};
                int red = static_cast<int>(center[0]) * 4;
                int green = static_cast<int>(center[1]) * 4;
                int blue = static_cast<int>(center[2]) * 4;
                int weight = 4;
                for (const uchar* sample : samples) {
                    if (rgbaChannelDistance(center, sample) > edgeThreshold) continue;
                    red += sample[0]; green += sample[1]; blue += sample[2];
                    ++weight;
                }
                uchar* output = destination + x * 4;
                output[0] = static_cast<uchar>(red / weight);
                output[1] = static_cast<uchar>(green / weight);
                output[2] = static_cast<uchar>(blue / weight);
                output[3] = 255;
            }
        }
    }

    static void temporalAccumulate(QImage& current, const QImage& history, float historyWeight)
    {
        if (current.format() != QImage::Format_RGBA8888 ||
            history.format() != QImage::Format_RGBA8888) return;
        const int oldWeight = std::clamp(static_cast<int>(historyWeight * 256.0f), 0, 255);
        const int newWeight = 256 - oldWeight;
        for (int y = 0; y < current.height(); ++y) {
            uchar* output = current.scanLine(y);
            const uchar* previous = history.constScanLine(y);
            for (int x = 0; x < current.width(); ++x) {
                uchar* now = output + x * 4;
                const uchar* old = previous + x * 4;
                // Reject history across a large lighting or silhouette change.
                if (rgbaChannelDistance(now, old) > 96) continue;
                now[0] = static_cast<uchar>((static_cast<int>(now[0]) * newWeight +
                                             static_cast<int>(old[0]) * oldWeight) >> 8);
                now[1] = static_cast<uchar>((static_cast<int>(now[1]) * newWeight +
                                             static_cast<int>(old[1]) * oldWeight) >> 8);
                now[2] = static_cast<uchar>((static_cast<int>(now[2]) * newWeight +
                                             static_cast<int>(old[2]) * oldWeight) >> 8);
                now[3] = 255;
            }
        }
    }

private:
    struct Buffer
    {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{0};
        VkDeviceAddress address{0};
    };

    struct AccelerationStructure
    {
        VkAccelerationStructureKHR handle{VK_NULL_HANDLE};
        Buffer storage;
        VkDeviceAddress address{0};
    };

    // VRAD radiosity bounce solve.
    //
    // Cosine-distributed rays per patch. Every surviving transfer carries the
    // same weight, so this is both the form-factor resolution and the per-patch
    // memory cost: patches * this * 8 bytes. 32 keeps a MAX_PATCHES map at
    // roughly 67 MB, which is the budget that keeps the bake affordable.
    static constexpr std::uint32_t kTransfersPerPatch = 32;
    // Iterations of Bi = Ei + Pi * SUM(Bj * Fij). Once the transfer list exists
    // each bounce is pure buffer math over the patch array, so this is cheap
    // compared to the single ray-traced pass that built the list.
    static constexpr std::uint32_t kBounceCount = 64;
    // Indirect rays per prop corner. The hemisphere is stratified, so this trades
    // bake time for how cleanly a prop's bounce follows the geometry around it.
    static constexpr std::uint32_t kPropIndirectRays = 32;
    // Three corners x six axes x 16 bytes = 288 bytes per prop triangle. The cap
    // is what keeps a prop-dense map from turning a lighting improvement into an
    // out-of-memory failure; past it, props keep the ambient approximation.
    static constexpr VkDeviceSize kPropCubeBytesPerTriangle = 3 * 6 * 16;
    static constexpr VkDeviceSize kMaximumPropCubeBytes = 256ull * 1024ull * 1024ull;

    struct RadiosityResources
    {
        Buffer patches;
        Buffer patchRects;
        Buffer patchIndexGrid;
        Buffer transfers;
        Buffer patchDirect;
        Buffer patchReflectivity;
        Buffer patchBounceA;
        Buffer patchBounceB;
        Buffer patchTotal;
        Buffer control;
        // rad_common.glsl declares one descriptor set shared by all six bake
        // passes. The patch-only solve never dispatches the luxel or prop
        // passes, but the bindings they use still have to be filled for the set
        // to be complete, so these stay as minimum-size placeholders.
        Buffer luxels;
        Buffer luxelRects;
        Buffer propCube;
        VkImage lightmapImage{VK_NULL_HANDLE};
        VkDeviceMemory lightmapMemory{VK_NULL_HANDLE};
        VkImageView lightmapView{VK_NULL_HANDLE};

        VkDescriptorSetLayout descriptorLayout{VK_NULL_HANDLE};
        VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
        VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
        VkShaderModule patchDirectModule{VK_NULL_HANDLE};
        VkShaderModule transfersModule{VK_NULL_HANDLE};
        VkShaderModule bounceModule{VK_NULL_HANDLE};
        VkShaderModule propVertexModule{VK_NULL_HANDLE};
        VkPipeline patchDirectPipeline{VK_NULL_HANDLE};
        VkPipeline transfersPipeline{VK_NULL_HANDLE};
        VkPipeline bouncePipeline{VK_NULL_HANDLE};
        VkPipeline propVertexPipeline{VK_NULL_HANDLE};

        std::uint32_t patchCount{0};
        // Static-prop triangles carrying an ambient-cube slot. Zero disables the
        // prop pass and leaves props on the preview's ambient approximation.
        std::uint32_t propTriangleCount{0};
        bool ready{false};
    };

    int scorePhysicalDevice(VkPhysicalDevice candidate,
                            VkPhysicalDeviceProperties& properties,
                            std::uint32_t& queueFamily,
                            VkDeviceSize& scratchAlignment)
    {
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (VK_API_VERSION_MAJOR(properties.apiVersion) < 1 ||
            (VK_API_VERSION_MAJOR(properties.apiVersion) == 1 &&
             VK_API_VERSION_MINOR(properties.apiVersion) < 2))
            return std::numeric_limits<int>::min();

        // Never silently run the expensive preview on a CPU Vulkan driver.
        // Mesa lavapipe/LLVMpipe can expose compute but is not an RTX path.
        const QString deviceName = QString::fromUtf8(properties.deviceName).toLower();
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
            deviceName.contains(QStringLiteral("llvmpipe")) ||
            deviceName.contains(QStringLiteral("lavapipe")) ||
            deviceName.contains(QStringLiteral("software")))
            return std::numeric_limits<int>::min();

        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount,
                                              extensions.data());
        std::set<std::string> names;
        for (const auto& extension : extensions) names.insert(extension.extensionName);
        if (!names.contains(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ||
            !names.contains(VK_KHR_RAY_QUERY_EXTENSION_NAME) ||
            !names.contains(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME))
            return std::numeric_limits<int>::min();

        VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddress{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &bufferAddress;
        bufferAddress.pNext = &acceleration;
        acceleration.pNext = &rayQuery;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (!bufferAddress.bufferDeviceAddress || !acceleration.accelerationStructure ||
            !rayQuery.rayQuery)
            return std::numeric_limits<int>::min();

        std::uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
        bool foundQueue = false;
        for (std::uint32_t index = 0; index < queueCount; ++index) {
            if ((queues[index].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) continue;
            queueFamily = index;
            foundQueue = true;
            break;
        }
        if (!foundQueue) return std::numeric_limits<int>::min();

        VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &accelerationProperties;
        vkGetPhysicalDeviceProperties2(candidate, &properties2);
        scratchAlignment = std::max<VkDeviceSize>(
            256, accelerationProperties.minAccelerationStructureScratchOffsetAlignment);

        int score = 0;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 100000;
        else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 10000;
        else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score += 1000;
        score += static_cast<int>(properties.limits.maxComputeSharedMemorySize / 1024u);
        score += static_cast<int>(properties.limits.maxImageDimension2D / 1024u);
        return score;
    }

    std::optional<std::uint32_t> memoryType(std::uint32_t mask,
                                            VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);
        for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
            if ((mask & (1u << index)) != 0u &&
                (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties)
                return index;
        }
        return std::nullopt;
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties, bool deviceAddress,
                      Buffer& output, QString& error, const void* initialData = nullptr)
    {
        destroyBuffer(output);
        output.size = std::max<VkDeviceSize>(size, 16);
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = output.size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateBuffer(device_, &bufferInfo, nullptr, &output.buffer);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create Vulkan buffer: %1").arg(vkResultText(result));
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, output.buffer, &requirements);
        const auto type = memoryType(requirements.memoryTypeBits, properties);
        if (!type) {
            error = QStringLiteral("No compatible Vulkan buffer memory type");
            return false;
        }
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = deviceAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = deviceAddress ? &flags : nullptr;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *type;
        result = vkAllocateMemory(device_, &allocation, nullptr, &output.memory);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not allocate Vulkan buffer memory: %1")
                        .arg(vkResultText(result));
            return false;
        }
        result = vkBindBufferMemory(device_, output.buffer, output.memory, 0);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not bind Vulkan buffer memory: %1")
                        .arg(vkResultText(result));
            return false;
        }
        if (deviceAddress) {
            VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            addressInfo.buffer = output.buffer;
            output.address = vkGetBufferDeviceAddress(device_, &addressInfo);
        }
        if (initialData && size > 0) {
            void* mapped = nullptr;
            result = vkMapMemory(device_, output.memory, 0, size, 0, &mapped);
            if (result != VK_SUCCESS || !mapped) {
                error = QStringLiteral("Could not map Vulkan upload buffer: %1")
                            .arg(vkResultText(result));
                return false;
            }
            std::memcpy(mapped, initialData, static_cast<std::size_t>(size));
            vkUnmapMemory(device_, output.memory);
        }
        return true;
    }

    void destroyBuffer(Buffer& buffer)
    {
        if (!device_) return;
        if (buffer.buffer) vkDestroyBuffer(device_, buffer.buffer, nullptr);
        if (buffer.memory) vkFreeMemory(device_, buffer.memory, nullptr);
        buffer = {};
    }

    bool beginCommands(QString& error)
    {
        vkResetCommandBuffer(commandBuffer_, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult result = vkBeginCommandBuffer(commandBuffer_, &begin);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not begin Vulkan commands: %1").arg(vkResultText(result));
            return false;
        }
        return true;
    }

    bool submitCommands(QString& error)
    {
        VkResult result = vkEndCommandBuffer(commandBuffer_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not end Vulkan commands: %1").arg(vkResultText(result));
            return false;
        }
        vkResetFences(device_, 1, &fence_);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        result = vkQueueSubmit(queue_, 1, &submit, fence_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not submit Vulkan commands: %1").arg(vkResultText(result));
            return false;
        }
        result = vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                                 std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Vulkan rendering did not complete: %1").arg(vkResultText(result));
            return false;
        }
        return true;
    }

    bool createImage(std::uint32_t width, std::uint32_t height, VkFormat format,
                     VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory,
                     VkImageView& view, QString& error, std::uint32_t arrayLayers = 1)
    {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = std::max(1u, arrayLayers);
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vkCreateImage(device_, &imageInfo, nullptr, &image);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create Vulkan image: %1").arg(vkResultText(result));
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, image, &requirements);
        const auto type = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!type) {
            error = QStringLiteral("No compatible device-local Vulkan image memory");
            return false;
        }
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *type;
        result = vkAllocateMemory(device_, &allocation, nullptr, &memory);
        if (result != VK_SUCCESS || vkBindImageMemory(device_, image, memory, 0) != VK_SUCCESS) {
            error = QStringLiteral("Could not allocate/bind Vulkan image memory");
            return false;
        }
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                 : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = std::max(1u, arrayLayers);
        result = vkCreateImageView(device_, &viewInfo, nullptr, &view);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create Vulkan image view: %1").arg(vkResultText(result));
            return false;
        }
        return true;
    }

    void destroyImage(VkImage& image, VkDeviceMemory& memory, VkImageView& view)
    {
        if (!device_) return;
        if (view) vkDestroyImageView(device_, view, nullptr);
        if (image) vkDestroyImage(device_, image, nullptr);
        if (memory) vkFreeMemory(device_, memory, nullptr);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;
    }

    bool createPipeline(QString& error)
    {
        std::array<VkDescriptorSetLayoutBinding, 18> bindings{};
        const std::array<VkDescriptorType, 18> types{
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            // Radiosity patch grid: rects, index grid, bounced light.
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            // Linear HDR frame and the two quarter-resolution bloom buffers.
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            // Static-prop ambient cubes from the radiosity bake.
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        };
        for (std::uint32_t index = 0; index < bindings.size(); ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType = types[index];
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        VkResult result = vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                                      &descriptorLayout_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create ray-query descriptor layout: %1")
                        .arg(vkResultText(result));
            return false;
        }
        // bloom_blur.comp picks its direction from a push constant, so the
        // shared layout has to carry a range even though the other passes
        // ignore it.
        VkPushConstantRange postPush{};
        postPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        postPush.offset = 0;
        postPush.size = sizeof(std::uint32_t);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorLayout_;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &postPush;
        result = vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create ray-query pipeline layout: %1")
                        .arg(vkResultText(result));
            return false;
        }

        QFile shaderFile(QStringLiteral(":/shaders/raytraced_preview.comp.spv"));
        if (!shaderFile.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("The compiled raytraced_preview.comp.spv resource is missing");
            return false;
        }
        const QByteArray shaderBytes = shaderFile.readAll();
        if (shaderBytes.isEmpty() || (shaderBytes.size() % 4) != 0) {
            error = QStringLiteral("The ray-traced preview SPIR-V resource is invalid");
            return false;
        }
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = static_cast<std::size_t>(shaderBytes.size());
        shaderInfo.pCode = reinterpret_cast<const std::uint32_t*>(shaderBytes.constData());
        result = vkCreateShaderModule(device_, &shaderInfo, nullptr, &shaderModule_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create ray-query shader module: %1")
                        .arg(vkResultText(result));
            return false;
        }
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shaderModule_;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = pipelineLayout_;
        result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                          nullptr, &pipeline_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create Vulkan ray-query pipeline: %1")
                        .arg(vkResultText(result));
            return false;
        }

        // Source's HDR display chain: bright pass, separable blur, composite.
        struct PostPass
        {
            const char* resource;
            VkShaderModule* module;
            VkPipeline* pipeline;
        };
        const std::array<PostPass, 3> post{{
            {":/shaders/bloom_bright.comp.spv", &brightModule_, &brightPipeline_},
            {":/shaders/bloom_blur.comp.spv", &blurModule_, &blurPipeline_},
            {":/shaders/composite.comp.spv", &compositeModule_, &compositePipeline_},
        }};
        for (const PostPass& pass : post) {
            if (!createRadiosityShaderModule(QString::fromLatin1(pass.resource),
                                             *pass.module, error))
                return false;
            VkPipelineShaderStageCreateInfo postStage{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            postStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            postStage.module = *pass.module;
            postStage.pName = "main";
            VkComputePipelineCreateInfo postInfo{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            postInfo.stage = postStage;
            postInfo.layout = pipelineLayout_;
            result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &postInfo,
                                              nullptr, pass.pipeline);
            if (result != VK_SUCCESS) {
                error = QStringLiteral("Could not create %1 pipeline: %2")
                            .arg(QString::fromLatin1(pass.resource), vkResultText(result));
                return false;
            }
        }
        return true;
    }

    bool createRadiosityShaderModule(const QString& resource, VkShaderModule& output,
                                     QString& error)
    {
        QFile file(resource);
        if (!file.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("The compiled %1 resource is missing").arg(resource);
            return false;
        }
        const QByteArray bytes = file.readAll();
        if (bytes.isEmpty() || (bytes.size() % 4) != 0) {
            error = QStringLiteral("%1 is not valid SPIR-V").arg(resource);
            return false;
        }
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = static_cast<std::size_t>(bytes.size());
        info.pCode = reinterpret_cast<const std::uint32_t*>(bytes.constData());
        const VkResult result = vkCreateShaderModule(device_, &info, nullptr, &output);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create %1 module: %2")
                        .arg(resource, vkResultText(result));
            return false;
        }
        return true;
    }

    // The 21-binding set rad_common.glsl declares, shared by every bake pass.
    bool createRadiosityPipelines(QString& error)
    {
        std::array<VkDescriptorSetLayoutBinding, 21> bindings{};
        const std::array<VkDescriptorType, 21> types{
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,  // 0  scene TLAS
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 1  vertices
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 2  indices
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 3  triangles
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 4  materials
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,      // 5  material atlas
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 6  map lights
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 7  luxels
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 8  patches
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 9  luxel rects
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 10 patch rects
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 11 transfers
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 12 patch direct
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 13 patch reflectivity
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 14 bounce A
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 15 bounce B
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 16 patch total
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,               // 17 lightmap image
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,              // 18 control
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 19 patch index grid
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,              // 20 prop cube
        };
        for (std::uint32_t index = 0; index < bindings.size(); ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType = types[index];
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        VkResult result = vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                                      &radiosity_.descriptorLayout);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create radiosity descriptor layout: %1")
                        .arg(vkResultText(result));
            return false;
        }

        // All six passes share one push range so they can share a layout.
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.offset = 0;
        push.size = sizeof(std::array<std::uint32_t, 4>);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &radiosity_.descriptorLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        result = vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr,
                                        &radiosity_.pipelineLayout);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create radiosity pipeline layout: %1")
                        .arg(vkResultText(result));
            return false;
        }

        struct PassModule
        {
            const char* resource;
            VkShaderModule* module;
            VkPipeline* pipeline;
        };
        const std::array<PassModule, 4> passes{{
            {":/shaders/rad_patch_direct.comp.spv", &radiosity_.patchDirectModule,
             &radiosity_.patchDirectPipeline},
            {":/shaders/rad_transfers.comp.spv", &radiosity_.transfersModule,
             &radiosity_.transfersPipeline},
            {":/shaders/rad_bounce.comp.spv", &radiosity_.bounceModule,
             &radiosity_.bouncePipeline},
            {":/shaders/rad_prop_vertex.comp.spv", &radiosity_.propVertexModule,
             &radiosity_.propVertexPipeline},
        }};
        for (const PassModule& pass : passes) {
            if (!createRadiosityShaderModule(QString::fromLatin1(pass.resource),
                                             *pass.module, error))
                return false;
            VkPipelineShaderStageCreateInfo stage{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = *pass.module;
            stage.pName = "main";
            VkComputePipelineCreateInfo pipelineInfo{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = stage;
            pipelineInfo.layout = radiosity_.pipelineLayout;
            result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                              nullptr, pass.pipeline);
            if (result != VK_SUCCESS) {
                error = QStringLiteral("Could not create radiosity pipeline %1: %2")
                            .arg(QString::fromLatin1(pass.resource), vkResultText(result));
                return false;
            }
        }

        std::array<VkDescriptorPoolSize, 4> sizes{};
        sizes[0] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
        sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 17};
        sizes[2] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        sizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        // One uniform buffer for the control block.
        std::array<VkDescriptorPoolSize, 5> allSizes{};
        std::copy(sizes.begin(), sizes.end(), allSizes.begin());
        allSizes[4] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(allSizes.size());
        poolInfo.pPoolSizes = allSizes.data();
        result = vkCreateDescriptorPool(device_, &poolInfo, nullptr, &radiosity_.descriptorPool);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create radiosity descriptor pool: %1")
                        .arg(vkResultText(result));
            return false;
        }
        VkDescriptorSetAllocateInfo allocation{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = radiosity_.descriptorPool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &radiosity_.descriptorLayout;
        result = vkAllocateDescriptorSets(device_, &allocation, &radiosity_.descriptorSet);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not allocate radiosity descriptor set: %1")
                        .arg(vkResultText(result));
            return false;
        }
        return true;
    }

    void destroyRadiosityBuffers()
    {
        for (Buffer* buffer : {&radiosity_.patches, &radiosity_.patchRects,
                               &radiosity_.patchIndexGrid, &radiosity_.transfers,
                               &radiosity_.patchDirect, &radiosity_.patchReflectivity,
                               &radiosity_.patchBounceA, &radiosity_.patchBounceB,
                               &radiosity_.patchTotal, &radiosity_.control,
                               &radiosity_.luxels, &radiosity_.luxelRects,
                               &radiosity_.propCube})
            destroyBuffer(*buffer);
        destroyImage(radiosity_.lightmapImage, radiosity_.lightmapMemory,
                     radiosity_.lightmapView);
        radiosity_.patchCount = 0;
        radiosity_.propTriangleCount = 0;
        radiosity_.ready = false;
    }

    // Allocates the patch-grid buffers and points the shared descriptor set at
    // them. Returns false only on a hard Vulkan failure: a scene with no lit
    // faces simply leaves the solve disabled and the preview falls back to its
    // ambient approximation.
    bool uploadRadiosity(QString& error)
    {
        destroyRadiosityBuffers();
        const hammer::render::RadiosityData& data = scene_.radiosity;
        if (!data.patchesValid() || !radiosity_.descriptorSet) {
            qWarning("Radiosity disabled: patches=%zu rects=%zu grid=%zu set=%d status=%s",
                     data.patches.size(), data.patchRects.records.size(),
                     data.patchIndexGrid.size(), radiosity_.descriptorSet ? 1 : 0,
                     data.status.empty() ? "(none)" : data.status.c_str());
            return true;
        }

        const std::uint32_t patchCount = static_cast<std::uint32_t>(data.patches.size());
        const VkDeviceSize patchVectors = VkDeviceSize(patchCount) * 16;
        const VkDeviceSize transferBytes =
            VkDeviceSize(patchCount) * kTransfersPerPatch * 8;

        constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        constexpr VkMemoryPropertyFlags hostVisible =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        constexpr VkMemoryPropertyFlags deviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        const std::array<std::uint32_t, 4> placeholder{};
        if (!createBuffer(data.patches.size() * sizeof(data.patches[0]), storage,
                          hostVisible, false, radiosity_.patches, error, data.patches.data()) ||
            !createBuffer(data.patchRects.records.size() * sizeof(data.patchRects.records[0]),
                          storage, hostVisible, false, radiosity_.patchRects, error,
                          data.patchRects.records.data()) ||
            !createBuffer(data.patchIndexGrid.size() * sizeof(std::uint32_t), storage,
                          hostVisible, false, radiosity_.patchIndexGrid, error,
                          data.patchIndexGrid.data()) ||
            !createBuffer(transferBytes, storage, deviceLocal, false,
                          radiosity_.transfers, error) ||
            !createBuffer(patchVectors, storage, deviceLocal, false,
                          radiosity_.patchDirect, error) ||
            !createBuffer(patchVectors, storage, deviceLocal, false,
                          radiosity_.patchReflectivity, error) ||
            !createBuffer(patchVectors, storage, deviceLocal, false,
                          radiosity_.patchBounceA, error) ||
            !createBuffer(patchVectors, storage, deviceLocal, false,
                          radiosity_.patchBounceB, error) ||
            !createBuffer(patchVectors, storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          deviceLocal, false, radiosity_.patchTotal, error) ||
            // Bindings the patch-only solve never reads, kept minimal.
            !createBuffer(16, storage, hostVisible, false, radiosity_.luxels, error,
                          placeholder.data()) ||
            !createBuffer(16, storage, hostVisible, false, radiosity_.luxelRects, error,
                          placeholder.data()))
            return false;

        // Static-prop ambient cubes. Props have no lightmap, so this is the only
        // place bounced light can live for them.
        const VkDeviceSize propBytes =
            VkDeviceSize(scene_.propCubeTriangles) * kPropCubeBytesPerTriangle;
        const std::uint32_t propTriangles = propBytes > 0 && propBytes <= kMaximumPropCubeBytes
            ? scene_.propCubeTriangles : 0u;
        if (propBytes > kMaximumPropCubeBytes)
            qWarning("Static-prop bounce lighting disabled: %u prop triangles need %.0f MB",
                     scene_.propCubeTriangles,
                     static_cast<double>(propBytes) / (1024.0 * 1024.0));
        if (!createBuffer(propTriangles > 0 ? propBytes : 16,
                          storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          propTriangles > 0 ? deviceLocal : hostVisible, false,
                          radiosity_.propCube, error,
                          propTriangles > 0 ? nullptr : placeholder.data()))
            return false;

        // std140 RadControlBuffer.
        struct RadControl
        {
            std::array<std::uint32_t, 4> counts{};
            std::array<std::uint32_t, 4> atlasSize{};
            std::array<float, 4> skyAmbient{};
            std::array<float, 4> skyColor{};
            std::array<float, 4> tuning{};
        };
        RadControl control{};
        control.counts = {0u, patchCount, kTransfersPerPatch, kBounceCount};
        control.atlasSize = {1u, 1u, static_cast<std::uint32_t>(data.patchLayout.width),
                             static_cast<std::uint32_t>(data.patchLayout.height)};
        // x = prop shadow slop, y = surface bias, z = global light scale.
        // The scale stays 1 so a patch and a pixel agree on what a light is
        // worth: the preview's own direct lighting applies no extra factor.
        control.tuning = {8.0f, 0.5f, 1.0f, 0.0f};
        if (!createBuffer(sizeof(RadControl), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          hostVisible, false, radiosity_.control, error, &control))
            return false;

        if (!createImage(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT,
                         radiosity_.lightmapImage, radiosity_.lightmapMemory,
                         radiosity_.lightmapView, error))
            return false;
        {
            // The patch-only solve never dispatches the passes that write this
            // image, but the descriptor still has to reference a valid one in
            // its shader-usable layout.
            QString transitionError;
            if (beginCommands(transitionError)) {
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = radiosity_.lightmapImage;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;
                vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &barrier);
                submitCommands(transitionError);
            }
        }

        VkWriteDescriptorSetAccelerationStructureKHR accelerationWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        accelerationWrite.accelerationStructureCount = 1;
        accelerationWrite.pAccelerationStructures = &topLevel_.handle;

        VkDescriptorImageInfo atlasInfo{atlasSampler_, atlasView_,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo lightmapInfo{VK_NULL_HANDLE, radiosity_.lightmapView,
                                           VK_IMAGE_LAYOUT_GENERAL};

        const std::array<VkBuffer, 21> buffers{
            VK_NULL_HANDLE, vertexBuffer_.buffer, indexBuffer_.buffer, triangleBuffer_.buffer,
            materialBuffer_.buffer, VK_NULL_HANDLE, lightBuffer_.buffer,
            radiosity_.luxels.buffer, radiosity_.patches.buffer, radiosity_.luxelRects.buffer,
            radiosity_.patchRects.buffer, radiosity_.transfers.buffer,
            radiosity_.patchDirect.buffer, radiosity_.patchReflectivity.buffer,
            radiosity_.patchBounceA.buffer, radiosity_.patchBounceB.buffer,
            radiosity_.patchTotal.buffer, VK_NULL_HANDLE, radiosity_.control.buffer,
            radiosity_.patchIndexGrid.buffer, radiosity_.propCube.buffer};

        std::array<VkDescriptorBufferInfo, 21> bufferInfos{};
        std::array<VkWriteDescriptorSet, 21> writes{};
        for (std::uint32_t index = 0; index < writes.size(); ++index) {
            writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[index].dstSet = radiosity_.descriptorSet;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            if (index == 0) {
                writes[index].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                writes[index].pNext = &accelerationWrite;
            } else if (index == 5) {
                writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[index].pImageInfo = &atlasInfo;
            } else if (index == 17) {
                writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[index].pImageInfo = &lightmapInfo;
            } else {
                bufferInfos[index] = {buffers[index], 0, VK_WHOLE_SIZE};
                writes[index].descriptorType = index == 18
                    ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[index].pBufferInfo = &bufferInfos[index];
            }
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);

        radiosity_.patchCount = patchCount;
        radiosity_.propTriangleCount = propTriangles;
        radiosity_.ready = true;
        qWarning("Radiosity: %u patches across %zu faces, transfers %.1f MB", patchCount,
              data.patchLayout.faces.size(),
              static_cast<double>(transferBytes) / (1024.0 * 1024.0));
        return true;
    }

    // Reads the solved patch light back and reports what it contains.
    //
    // A radiosity solve fails silently: every stage runs, every dispatch
    // succeeds, and the result is a buffer of zeros if the patch grid and the
    // triangles referencing it ever disagree. Summarising the output once per
    // bake turns that into a line in the log instead of a dark map with no
    // explanation.
    void reportRadiosityResult()
    {
        if (!radiosity_.ready || radiosity_.patchCount == 0) return;
        const VkDeviceSize bytes = VkDeviceSize(radiosity_.patchCount) * 16;
        Buffer staging;
        QString error;
        if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          false, staging, error))
            return;
        if (beginCommands(error)) {
            VkBufferCopy region{0, 0, bytes};
            vkCmdCopyBuffer(commandBuffer_, radiosity_.patchTotal.buffer, staging.buffer,
                            1, &region);
            submitCommands(error);
        }
        void* mapped = nullptr;
        if (vkMapMemory(device_, staging.memory, 0, bytes, 0, &mapped) == VK_SUCCESS && mapped) {
            const auto* values = static_cast<const float*>(mapped);
            std::size_t lit = 0;
            double total = 0.0;
            float peak = 0.0f;
            for (std::uint32_t patch = 0; patch < radiosity_.patchCount; ++patch) {
                const float r = values[patch * 4 + 0];
                const float g = values[patch * 4 + 1];
                const float b = values[patch * 4 + 2];
                const float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                if (!std::isfinite(luminance)) continue;
                if (luminance > 0.0001f) ++lit;
                total += luminance;
                peak = std::max(peak, luminance);
            }
            vkUnmapMemory(device_, staging.memory);
            qWarning("Radiosity solve: %zu/%u patches lit, mean %.4f, peak %.4f", lit,
                  radiosity_.patchCount,
                  total / std::max<double>(1.0, radiosity_.patchCount), peak);
            if (lit == 0)
                qWarning("Radiosity solve produced no light: check that lit faces carry "
                         "TriangleHasLightmap and that data[2] indexes patchRects.");
        }
        destroyBuffer(staging);
    }

    // Same failure mode as the patch solve: a prop bake that reaches no patches
    // produces a buffer of zeros and simply looks like props that were never lit.
    // Summarise it once per bake so that shows up as a log line instead.
    void reportPropCubeResult()
    {
        if (!radiosity_.ready || radiosity_.propTriangleCount == 0) return;
        const VkDeviceSize bytes =
            VkDeviceSize(radiosity_.propTriangleCount) * kPropCubeBytesPerTriangle;
        Buffer staging;
        QString error;
        if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          false, staging, error))
            return;
        if (beginCommands(error)) {
            VkBufferCopy region{0, 0, bytes};
            vkCmdCopyBuffer(commandBuffer_, radiosity_.propCube.buffer, staging.buffer,
                            1, &region);
            submitCommands(error);
        }
        void* mapped = nullptr;
        if (vkMapMemory(device_, staging.memory, 0, bytes, 0, &mapped) == VK_SUCCESS && mapped) {
            const auto* values = static_cast<const float*>(mapped);
            const std::uint32_t cubes = radiosity_.propTriangleCount * 3u;
            std::size_t lit = 0;
            double total = 0.0;
            for (std::uint32_t cube = 0; cube < cubes; ++cube) {
                double luminance = 0.0;
                for (std::uint32_t axis = 0; axis < 6; ++axis) {
                    const float* rgb = values + (cube * 6u + axis) * 4u;
                    luminance += 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
                }
                if (!std::isfinite(luminance)) continue;
                if (luminance > 0.0001) ++lit;
                total += luminance;
            }
            vkUnmapMemory(device_, staging.memory);
            qWarning("Static-prop bounce: %zu/%u cubes lit, mean %.4f, %.1f MB", lit, cubes,
                     total / std::max<double>(1.0, cubes),
                     static_cast<double>(bytes) / (1024.0 * 1024.0));
            if (lit == 0)
                qWarning("Static-prop bounce produced no light: prop rays reach only "
                         "patch-carrying surfaces, so a prop with nothing lightmapped "
                         "around it gathers nothing.");
        }
        destroyBuffer(staging);
    }

    // patch_direct -> transfers -> N x bounce. Recorded into one submission.
    void recordRadiosityBake(VkCommandBuffer commandBuffer)
    {
        if (!radiosity_.ready || radiosity_.patchCount == 0) return;
        const std::uint32_t groups = (radiosity_.patchCount + 63u) / 64u;
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                radiosity_.pipelineLayout, 0, 1,
                                &radiosity_.descriptorSet, 0, nullptr);

        const auto barrier = [&]() {
            VkMemoryBarrier memoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memoryBarrier,
                                 0, nullptr, 0, nullptr);
        };
        const auto dispatch = [&](VkPipeline pipeline, std::uint32_t bounceIndex) {
            const std::array<std::uint32_t, 4> push{bounceIndex, 0u, 0u, 0u};
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdPushConstants(commandBuffer, radiosity_.pipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push.data());
            vkCmdDispatch(commandBuffer, groups, 1, 1);
        };

        dispatch(radiosity_.patchDirectPipeline, 0u);
        barrier();
        dispatch(radiosity_.transfersPipeline, 0u);
        barrier();
        // Even bounces read A and write B, odd the reverse; the index has to be
        // a push constant because every iteration is in one submission.
        for (std::uint32_t bounce = 0; bounce < kBounceCount; ++bounce) {
            dispatch(radiosity_.bouncePipeline, bounce);
            barrier();
        }

        // Static props last: it samples the solved patch grid, so it can only run
        // once the bounces have converged. One invocation per triangle, skipping
        // straight back out for anything that is not a prop, which is why the
        // dispatch covers the whole triangle buffer rather than a prop list.
        if (radiosity_.propTriangleCount == 0) return;
        vkCmdFillBuffer(commandBuffer, radiosity_.propCube.buffer, 0,
                        VkDeviceSize(radiosity_.propTriangleCount) * kPropCubeBytesPerTriangle,
                        0u);
        VkMemoryBarrier fillBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fillBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fillBarrier,
                             0, nullptr, 0, nullptr);

        const std::uint32_t triangleCount = static_cast<std::uint32_t>(scene_.triangles.size());
        const std::array<std::uint32_t, 4> propPush{0u, triangleCount, kPropIndirectRays, 0u};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          radiosity_.propVertexPipeline);
        vkCmdPushConstants(commandBuffer, radiosity_.pipelineLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(propPush), propPush.data());
        vkCmdDispatch(commandBuffer, (triangleCount + 63u) / 64u, 1, 1);
        barrier();
    }

    bool uploadScene(QString& error)
    {
        vkDeviceWaitIdle(device_);
        destroySceneResources();
        descriptorSet_ = VK_NULL_HANDLE;

        const VkMemoryPropertyFlags uploadMemory =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!createBuffer(scene_.vertices.size() * sizeof(scene_.vertices[0]),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                          uploadMemory, true, vertexBuffer_, error, scene_.vertices.data()) ||
            !createBuffer(scene_.indices.size() * sizeof(scene_.indices[0]),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                          uploadMemory, true, indexBuffer_, error, scene_.indices.data()) ||
            !createBuffer(scene_.triangles.size() * sizeof(scene_.triangles[0]),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, uploadMemory, false,
                          triangleBuffer_, error, scene_.triangles.data()) ||
            !createBuffer(scene_.materials.size() * sizeof(scene_.materials[0]),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, uploadMemory, false,
                          materialBuffer_, error, scene_.materials.data()) ||
            !createBuffer(scene_.lights.size() * sizeof(scene_.lights[0]),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, uploadMemory, false,
                          lightBuffer_, error, scene_.lights.data()) ||
            !createBuffer(std::max<std::size_t>(1, scene_.colorCorrectionLutTexels.size()) * sizeof(std::uint32_t),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, uploadMemory, false,
                          colorCorrectionBuffer_, error,
                          scene_.colorCorrectionLutTexels.empty() ? nullptr :
                              scene_.colorCorrectionLutTexels.data()) ||
            !createBuffer(17u * sizeof(std::uint32_t),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          uploadMemory, false, exposureHistogramBuffer_, error) ||
            !createBuffer(sizeof(CameraGpu), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          uploadMemory, false, cameraBuffer_, error)) return false;

        if (!uploadAtlas(error)) return false;
        if (!buildBottomLevel(error) || !buildTopLevel(error)) return false;
        if (!updateDescriptors(error)) return false;
        // The bounce solve traces against the finished acceleration structure,
        // so it can only run once the scene upload is complete. It is a whole
        // separate submission and it is deliberately tied to scene rebuilds
        // rather than to frames: nothing here may run per frame.
        if (!uploadRadiosity(error)) return false;
        if (radiosity_.ready) {
            QString bakeError;
            if (beginCommands(bakeError)) {
                recordRadiosityBake(commandBuffer_);
                submitCommands(bakeError);
            }
            reportRadiosityResult();
            reportPropCubeResult();
            // The preview's own descriptors were written before the patch
            // buffers existed, so they still point at the placeholder. Rewrite
            // them now that the solve has real results.
            if (!updateDescriptors(error)) return false;
        }
        return true;
    }

    bool uploadAtlas(QString& error)
    {
        Buffer staging;
        if (!createBuffer(scene_.atlas.rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          false, staging, error, scene_.atlas.rgba.data())) return false;
        if (!createImage(static_cast<std::uint32_t>(scene_.atlas.width),
                         static_cast<std::uint32_t>(scene_.atlas.height),
                         VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         atlasImage_, atlasMemory_, atlasView_, error,
                         static_cast<std::uint32_t>(scene_.atlas.layers))) {
            destroyBuffer(staging);
            return false;
        }
        if (!beginCommands(error)) { destroyBuffer(staging); return false; }
        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = atlasImage_;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.layerCount =
            static_cast<std::uint32_t>(scene_.atlas.layers);
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toTransfer);
        std::vector<VkBufferImageCopy> copies(
            static_cast<std::size_t>(scene_.atlas.layers));
        const VkDeviceSize layerBytes = static_cast<VkDeviceSize>(scene_.atlas.width) *
                                        scene_.atlas.height * 4u;
        for (std::uint32_t layer = 0;
             layer < static_cast<std::uint32_t>(scene_.atlas.layers); ++layer) {
            VkBufferImageCopy& copy = copies[static_cast<std::size_t>(layer)];
            copy.bufferOffset = layerBytes * layer;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.baseArrayLayer = layer;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {static_cast<std::uint32_t>(scene_.atlas.width),
                                static_cast<std::uint32_t>(scene_.atlas.height), 1};
        }
        vkCmdCopyBufferToImage(commandBuffer_, staging.buffer, atlasImage_,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(copies.size()), copies.data());
        VkImageMemoryBarrier toSample = toTransfer;
        toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toSample);
        const bool ok = submitCommands(error);
        destroyBuffer(staging);
        if (!ok) return false;

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        VkResult result = vkCreateSampler(device_, &samplerInfo, nullptr, &atlasSampler_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create Vulkan material-atlas sampler: %1")
                        .arg(vkResultText(result));
            return false;
        }
        return true;
    }

    bool createAccelerationStructure(VkAccelerationStructureTypeKHR type,
                                     const VkAccelerationStructureBuildGeometryInfoKHR& templateInfo,
                                     const std::uint32_t* primitiveCount,
                                     AccelerationStructure& output,
                                     QString& error)
    {
        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        getAccelerationStructureBuildSizes_(device_,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &templateInfo, primitiveCount, &sizes);
        if (!createBuffer(sizes.accelerationStructureSize,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true,
                          output.storage, error)) return false;
        VkAccelerationStructureCreateInfoKHR createInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        createInfo.buffer = output.storage.buffer;
        createInfo.size = sizes.accelerationStructureSize;
        createInfo.type = type;
        VkResult result = createAccelerationStructure_(device_, &createInfo, nullptr, &output.handle);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create Vulkan acceleration structure: %1")
                        .arg(vkResultText(result));
            return false;
        }

        Buffer scratch;
        if (!createBuffer(sizes.buildScratchSize + scratchAlignment_,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, scratch, error)) return false;
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo = templateInfo;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.dstAccelerationStructure = output.handle;
        buildInfo.scratchData.deviceAddress = alignUp(scratch.address, scratchAlignment_);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = *primitiveCount;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};
        if (!beginCommands(error)) { destroyBuffer(scratch); return false; }
        commandBuildAccelerationStructures_(commandBuffer_, 1, &buildInfo, ranges);
        const bool ok = submitCommands(error);
        destroyBuffer(scratch);
        if (!ok) return false;

        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addressInfo.accelerationStructure = output.handle;
        output.address = getAccelerationStructureDeviceAddress_(device_, &addressInfo);
        return output.address != 0;
    }

    bool buildBottomLevel(QString& error)
    {
        VkAccelerationStructureGeometryTrianglesDataKHR trianglesData{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        trianglesData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        trianglesData.vertexData.deviceAddress = vertexBuffer_.address;
        trianglesData.vertexStride = sizeof(hammer::render::RayTracingVertex);
        trianglesData.maxVertex = static_cast<std::uint32_t>(scene_.vertices.size() - 1);
        trianglesData.indexType = VK_INDEX_TYPE_UINT32;
        trianglesData.indexData.deviceAddress = indexBuffer_.address;
        VkAccelerationStructureGeometryKHR geometry{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.geometry.triangles = trianglesData;
        geometry.flags = 0; // Alpha-tested and translucent candidates stay non-opaque.
        VkAccelerationStructureBuildGeometryInfoKHR build{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geometry;
        const std::uint32_t primitiveCount = static_cast<std::uint32_t>(scene_.indices.size() / 3);
        return createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                           build, &primitiveCount, bottomLevel_, error);
    }

    bool uploadDynamicVertices(QString& error)
    {
        const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(scene_.vertices.size()) *
                                         sizeof(scene_.vertices[0]);
        if (!vertexBuffer_.buffer || vertexBuffer_.size != vertexBytes || !bottomLevel_.handle) {
            error = QStringLiteral("Dynamic RT vertex layout changed");
            return false;
        }
        void* mapped = nullptr;
        VkResult result = vkMapMemory(device_, vertexBuffer_.memory, 0, vertexBytes, 0, &mapped);
        if (result != VK_SUCCESS || !mapped) {
            error = QStringLiteral("Could not map dynamic RT vertex buffer: %1")
                        .arg(vkResultText(result));
            return false;
        }
        std::memcpy(mapped, scene_.vertices.data(), static_cast<std::size_t>(vertexBytes));
        vkUnmapMemory(device_, vertexBuffer_.memory);

        VkAccelerationStructureGeometryTrianglesDataKHR trianglesData{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        trianglesData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        trianglesData.vertexData.deviceAddress = vertexBuffer_.address;
        trianglesData.vertexStride = sizeof(hammer::render::RayTracingVertex);
        trianglesData.maxVertex = static_cast<std::uint32_t>(scene_.vertices.size() - 1);
        trianglesData.indexType = VK_INDEX_TYPE_UINT32;
        trianglesData.indexData.deviceAddress = indexBuffer_.address;
        VkAccelerationStructureGeometryKHR geometry{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.geometry.triangles = trianglesData;
        geometry.flags = 0;
        VkAccelerationStructureBuildGeometryInfoKHR build{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        build.srcAccelerationStructure = bottomLevel_.handle;
        build.dstAccelerationStructure = bottomLevel_.handle;
        build.geometryCount = 1;
        build.pGeometries = &geometry;
        const std::uint32_t primitiveCount = static_cast<std::uint32_t>(scene_.indices.size() / 3);

        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        getAccelerationStructureBuildSizes_(device_,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &build, &primitiveCount, &sizes);
        const VkDeviceSize requiredScratch = sizes.updateScratchSize + scratchAlignment_;
        if (!updateScratchBuffer_.buffer || updateScratchBuffer_.size < requiredScratch) {
            destroyBuffer(updateScratchBuffer_);
            if (!createBuffer(requiredScratch,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true,
                              updateScratchBuffer_, error)) return false;
        }
        build.scratchData.deviceAddress = alignUp(updateScratchBuffer_.address, scratchAlignment_);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = primitiveCount;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};
        if (!beginCommands(error)) return false;

        VkBufferMemoryBarrier hostToBuild{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostToBuild.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        hostToBuild.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        hostToBuild.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostToBuild.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostToBuild.buffer = vertexBuffer_.buffer;
        hostToBuild.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             0, 0, nullptr, 1, &hostToBuild, 0, nullptr);
        commandBuildAccelerationStructures_(commandBuffer_, 1, &build, ranges);

        VkMemoryBarrier buildToTrace{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        buildToTrace.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        buildToTrace.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                     VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer_,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &buildToTrace, 0, nullptr, 0, nullptr);
        return submitCommands(error);
    }

    bool buildTopLevel(QString& error)
    {
        VkAccelerationStructureInstanceKHR instance{};
        instance.transform.matrix[0][0] = 1.0f;
        instance.transform.matrix[1][1] = 1.0f;
        instance.transform.matrix[2][2] = 1.0f;
        instance.mask = 0xffu;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = bottomLevel_.address;
        if (!createBuffer(sizeof(instance),
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          true, instanceBuffer_, error, &instance)) return false;
        VkAccelerationStructureGeometryInstancesDataKHR instances{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        instances.arrayOfPointers = VK_FALSE;
        instances.data.deviceAddress = instanceBuffer_.address;
        VkAccelerationStructureGeometryKHR geometry{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances = instances;
        VkAccelerationStructureBuildGeometryInfoKHR build{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geometry;
        const std::uint32_t primitiveCount = 1;
        return createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                                           build, &primitiveCount, topLevel_, error);
    }

    bool createOutput(int width, int height, QString& error)
    {
        vkDeviceWaitIdle(device_);
        destroyImage(outputImage_, outputMemory_, outputView_);
        destroyImage(hdrImage_, hdrMemory_, hdrView_);
        destroyImage(bloomImageA_, bloomMemoryA_, bloomViewA_);
        destroyImage(bloomImageB_, bloomMemoryB_, bloomViewB_);
        destroyBuffer(readback_);
        descriptorSet_ = VK_NULL_HANDLE;
        outputWidth_ = width;
        outputHeight_ = height;
        if (!createImage(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                         VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         outputImage_, outputMemory_, outputView_, error) ||
            !createBuffer(static_cast<VkDeviceSize>(width) * height * 4u,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          false, readback_, error)) return false;
        // Source downsamples the bright pass to a quarter of each axis.
        bloomWidth_ = std::max(1, width / 4);
        bloomHeight_ = std::max(1, height / 4);
        if (!createImage(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                         VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT,
                         hdrImage_, hdrMemory_, hdrView_, error) ||
            !createImage(static_cast<std::uint32_t>(bloomWidth_),
                         static_cast<std::uint32_t>(bloomHeight_),
                         VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT,
                         bloomImageA_, bloomMemoryA_, bloomViewA_, error) ||
            !createImage(static_cast<std::uint32_t>(bloomWidth_),
                         static_cast<std::uint32_t>(bloomHeight_),
                         VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT,
                         bloomImageB_, bloomMemoryB_, bloomViewB_, error)) return false;
        if (!beginCommands(error)) return false;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        for (VkImage image : {outputImage_, hdrImage_, bloomImageA_, bloomImageB_}) {
            barrier.image = image;
            vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &barrier);
        }
        if (!submitCommands(error)) return false;
        return updateDescriptors(error);
    }

    bool updateDescriptors(QString& error)
    {
        if (!outputView_ || !topLevel_.handle || !atlasView_ || !cameraBuffer_.buffer ||
            !hdrView_ || !bloomViewA_ || !bloomViewB_)
            return true;
        if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
        descriptorSet_ = VK_NULL_HANDLE;
        std::array<VkDescriptorPoolSize, 5> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        VkResult result = vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not create Vulkan descriptor pool: %1")
                        .arg(vkResultText(result));
            return false;
        }
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptorPool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &descriptorLayout_;
        result = vkAllocateDescriptorSets(device_, &allocate, &descriptorSet_);
        if (result != VK_SUCCESS) {
            error = QStringLiteral("Could not allocate Vulkan ray-query descriptor set: %1")
                        .arg(vkResultText(result));
            return false;
        }

        VkDescriptorImageInfo outputInfo{};
        outputInfo.imageView = outputView_;
        outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo atlasInfo{};
        atlasInfo.sampler = atlasSampler_;
        atlasInfo.imageView = atlasView_;
        atlasInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const std::array<VkDescriptorBufferInfo, 8> bufferInfos{{
            {vertexBuffer_.buffer, 0, VK_WHOLE_SIZE},
            {indexBuffer_.buffer, 0, VK_WHOLE_SIZE},
            {triangleBuffer_.buffer, 0, VK_WHOLE_SIZE},
            {materialBuffer_.buffer, 0, VK_WHOLE_SIZE},
            {cameraBuffer_.buffer, 0, sizeof(CameraGpu)},
            {lightBuffer_.buffer, 0, VK_WHOLE_SIZE},
            {colorCorrectionBuffer_.buffer, 0, VK_WHOLE_SIZE},
            {exposureHistogramBuffer_.buffer, 0, VK_WHOLE_SIZE},
        }};
        VkWriteDescriptorSetAccelerationStructureKHR accelerationWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        accelerationWrite.accelerationStructureCount = 1;
        accelerationWrite.pAccelerationStructures = &topLevel_.handle;
        // The radiosity buffers only exist once a scene with lit faces has been
        // uploaded. Before that, point the bindings at any live buffer so the
        // set is complete; radiosityControls.z keeps the shader from reading it.
        const VkBuffer patchRectsBuffer = radiosity_.patchRects.buffer
            ? radiosity_.patchRects.buffer : triangleBuffer_.buffer;
        const VkBuffer patchIndexBuffer = radiosity_.patchIndexGrid.buffer
            ? radiosity_.patchIndexGrid.buffer : triangleBuffer_.buffer;
        const VkBuffer patchTotalBuffer = radiosity_.patchTotal.buffer
            ? radiosity_.patchTotal.buffer : triangleBuffer_.buffer;
        const VkBuffer propCubeBuffer = radiosity_.propCube.buffer
            ? radiosity_.propCube.buffer : triangleBuffer_.buffer;
        const std::array<VkDescriptorBufferInfo, 4> radiosityInfos{{
            {patchRectsBuffer, 0, VK_WHOLE_SIZE},
            {patchIndexBuffer, 0, VK_WHOLE_SIZE},
            {patchTotalBuffer, 0, VK_WHOLE_SIZE},
            {propCubeBuffer, 0, VK_WHOLE_SIZE},
        }};
        const std::array<VkDescriptorImageInfo, 3> postImages{{
            {VK_NULL_HANDLE, hdrView_, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, bloomViewA_, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, bloomViewB_, VK_IMAGE_LAYOUT_GENERAL},
        }};
        std::array<VkWriteDescriptorSet, 18> writes{};
        for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = descriptorSet_;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &outputInfo;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[1].pNext = &accelerationWrite;
        for (std::uint32_t binding = 2; binding <= 5; ++binding) {
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo = &bufferInfos[binding - 2];
        }
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[6].pImageInfo = &atlasInfo;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[7].pBufferInfo = &bufferInfos[4];
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[8].pBufferInfo = &bufferInfos[5];
        writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[9].pBufferInfo = &bufferInfos[6];
        writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[10].pBufferInfo = &bufferInfos[7];
        for (std::uint32_t binding = 11; binding <= 13; ++binding) {
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo = &radiosityInfos[binding - 11];
        }
        for (std::uint32_t binding = 14; binding <= 16; ++binding) {
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[binding].pImageInfo = &postImages[binding - 14];
        }
        writes[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[17].pBufferInfo = &radiosityInfos[3];
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        return true;
    }

    static float sourceHistogramBoundary(int bucket)
    {
        const float normalized = std::clamp(static_cast<float>(bucket) / 16.0f, 0.0f, 1.0f);
        return std::pow(normalized, 1.5f);
    }

    static float findHistogramLocation(const std::array<std::uint32_t, 16>& bins,
                                       float percentBright, float snapTargetPercent = -1.0f)
    {
        std::uint64_t total = 0;
        for (std::uint32_t count : bins) total += count;
        if (total == 0) return -1.0f;

        const float desiredFraction = std::clamp(percentBright / 100.0f, 0.0f, 1.0f);
        float testedFraction = 0.0f;
        float testedRange = 0.0f;
        for (int bucket = 15; bucket >= 0; --bucket) {
            const float lower = sourceHistogramBoundary(bucket);
            const float upper = bucket == 15 ? 1.0f : sourceHistogramBoundary(bucket + 1);
            const float bucketRange = std::max(upper - lower, 0.000001f);
            const float bucketFraction = static_cast<float>(bins[static_cast<std::size_t>(bucket)]) /
                                         static_cast<float>(total);
            const float needed = desiredFraction - testedFraction;
            if (bucketFraction > 0.0f && bucketFraction >= needed) {
                if (snapTargetPercent >= 0.0f) {
                    const float snap = snapTargetPercent / 100.0f;
                    if (snap >= lower && snap <= upper) return snap;
                }
                const float withinBucket = std::clamp(needed / bucketFraction, 0.0f, 1.0f);
                const float location = 1.0f - (testedRange + bucketRange * withinBucket);
                return std::clamp(location, lower, upper);
            }
            testedFraction += bucketFraction;
            testedRange += bucketRange;
        }
        return 0.0f;
    }

    void updateToneMapFromHistogram()
    {
        if (!exposureHistogramBuffer_.memory) return;
        void* mapped = nullptr;
        if (vkMapMemory(device_, exposureHistogramBuffer_.memory, 0,
                        17u * sizeof(std::uint32_t), 0, &mapped) != VK_SUCCESS || !mapped)
            return;
        std::array<std::uint32_t, 17> histogram{};
        std::memcpy(histogram.data(), mapped, sizeof(histogram));
        vkUnmapMemory(device_, exposureHistogramBuffer_.memory);
        if (histogram[16] == 0u) return;

        std::array<std::uint32_t, 16> bins{};
        std::copy_n(histogram.begin(), 16, bins.begin());
        float brightLocation = findHistogramLocation(bins, 2.0f, 60.0f);
        if (brightLocation <= 0.0001f) brightLocation = 0.60f;
        float targetFactor = 0.60f / brightLocation;
        const float medianLocation = findHistogramLocation(bins, 50.0f);
        if (medianLocation > 0.0001f)
            targetFactor = std::max(targetFactor, 0.03f / medianLocation);

        const float minimum = std::max(0.001f,
            static_cast<float>(scene_.toneMap.autoExposureMin));
        const float maximum = std::max(minimum,
            static_cast<float>(scene_.toneMap.autoExposureMax));
        const float goal = std::clamp(currentToneScale_ * targetFactor, minimum, maximum);
        if (!toneMapTimer_.isValid()) toneMapTimer_.start();
        const float dt = std::clamp(static_cast<float>(toneMapTimer_.restart()) / 1000.0f,
                                    0.001f, 0.25f);
        const float authoredRate = std::max(0.01f, static_cast<float>(scene_.toneMap.rate));
        // Source adapts toward darkness faster than toward brightness; its default
        // down-adjust multiplier is 3.0.
        const float rate = authoredRate * (goal < currentToneScale_ ? 3.0f : 1.0f);
        const float blend = 1.0f - std::exp(-rate * dt);
        currentToneScale_ += (goal - currentToneScale_) * blend;
    }

    // Records the compute dispatch, submits it, and copies the output image
    // back into `frame`. Shared by the interactive viewport and the cubemap
    // bake so both render through exactly the same path.
    bool dispatchFrame(int renderWidth, int renderHeight, QImage& frame, QString& error)
    {
        if (!beginCommands(error)) return false;
        // Source-style auto exposure uses a 16-bin luminance histogram.  The
        // shader samples it sparsely, so this 68-byte clear/readback is tiny
        // compared with the ray-query workload.
        vkCmdFillBuffer(commandBuffer_, exposureHistogramBuffer_.buffer, 0, VK_WHOLE_SIZE, 0u);
        VkBufferMemoryBarrier histogramClear{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        histogramClear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        histogramClear.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        histogramClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        histogramClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        histogramClear.buffer = exposureHistogramBuffer_.buffer;
        histogramClear.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 1, &histogramClear, 0, nullptr);
        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
        vkCmdDispatch(commandBuffer_, (static_cast<std::uint32_t>(renderWidth) + 7u) / 8u,
                      (static_cast<std::uint32_t>(renderHeight) + 7u) / 8u, 1u);

        // Source's HDR display chain. The preview left linear light in
        // hdrImage_; this turns it into a displayable frame.
        //
        //   bright pass  quarter-res downsample keeping the bright end   -> A
        //   blur         horizontal A -> B, then vertical B -> A
        //   composite    exposure, + bloom, gamma, colour correction     -> output
        //
        // Order matters: bloom is gathered from pre-tonemap values, added to
        // the exposed frame, and only then converted to gamma and run through
        // the correction lookup, because the lookups were authored against
        // framebuffer values rather than linear ones.
        const auto imageBarrier = [&]() {
            VkMemoryBarrier memoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memoryBarrier,
                                 0, nullptr, 0, nullptr);
        };
        const std::uint32_t bloomGroupsX =
            (static_cast<std::uint32_t>(std::max(1, bloomWidth_)) + 7u) / 8u;
        const std::uint32_t bloomGroupsY =
            (static_cast<std::uint32_t>(std::max(1, bloomHeight_)) + 7u) / 8u;

        imageBarrier();
        // With bloom off the bright pass and both blurs are pure waste:
        // composite multiplies bloom by a zero scale. dispatchFrame is shared
        // with the cubemap bake, which also suppresses bloom, so the decision
        // travels on the flag updateCamera set rather than on the owner.
        if (bloomEnabled_) {
            vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, brightPipeline_);
            vkCmdDispatch(commandBuffer_, bloomGroupsX, bloomGroupsY, 1u);
            imageBarrier();
            vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, blurPipeline_);
            for (std::uint32_t vertical = 0; vertical < 2u; ++vertical) {
                vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                                   0, sizeof(vertical), &vertical);
                vkCmdDispatch(commandBuffer_, bloomGroupsX, bloomGroupsY, 1u);
                imageBarrier();
            }
        }
        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, compositePipeline_);
        vkCmdDispatch(commandBuffer_, (static_cast<std::uint32_t>(renderWidth) + 7u) / 8u,
                      (static_cast<std::uint32_t>(renderHeight) + 7u) / 8u, 1u);

        VkBufferMemoryBarrier histogramHostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        histogramHostRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        histogramHostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        histogramHostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        histogramHostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        histogramHostRead.buffer = exposureHistogramBuffer_.buffer;
        histogramHostRead.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0,
                             0, nullptr, 1, &histogramHostRead, 0, nullptr);

        VkImageMemoryBarrier toCopy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toCopy.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.image = outputImage_;
        toCopy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toCopy.subresourceRange.levelCount = 1;
        toCopy.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &toCopy);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {static_cast<std::uint32_t>(renderWidth),
                            static_cast<std::uint32_t>(renderHeight), 1u};
        vkCmdCopyImageToBuffer(commandBuffer_, outputImage_,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_.buffer, 1, &copy);
        VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.buffer = readback_.buffer;
        hostRead.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0,
                             0, nullptr, 1, &hostRead, 0, nullptr);

        VkImageMemoryBarrier toGeneral = toCopy;
        toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toGeneral);
        if (!submitCommands(error)) return false;
        updateToneMapFromHistogram();

        void* mapped = nullptr;
        VkResult result = vkMapMemory(device_, readback_.memory, 0,
                                      static_cast<VkDeviceSize>(renderWidth) * renderHeight * 4u,
                                      0, &mapped);
        if (result != VK_SUCCESS || !mapped) {
            error = QStringLiteral("Could not map Vulkan ray-traced framebuffer: %1")
                        .arg(vkResultText(result));
            return false;
        }
        frame = QImage(renderWidth, renderHeight, QImage::Format_RGBA8888);
        std::memcpy(frame.bits(), mapped,
                    static_cast<std::size_t>(renderWidth) * renderHeight * 4u);
        vkUnmapMemory(device_, readback_.memory);
        return true;
    }

public:
    // Renders every env_cubemap position in `probes` to six faces. The scene is
    // rebuilt once with entity helpers suppressed, so no editor billboard is
    // burned into a reflection, and is dropped afterwards so the interactive
    // viewport rebuilds its own (helper-carrying) scene on the next paint.
    bool bakeCubemaps(MapViewWidget& owner,
                      const std::vector<hammer::render::CubemapProbe>& probes,
                      std::vector<hammer::assets::CubeImage>& output, QString& error)
    {
        if (!initialize(error)) return false;
        if (probes.empty()) {
            error = QStringLiteral("The map contains no env_cubemap entities");
            return false;
        }

        historyFrame_ = QImage{};
        hammer::render::RayTracingBuildOptions options;
        options.hiddenToolTextures = owner.hiddenToolTextures_;
        options.displacementSolidMask = owner.displacementSolidMaskEnabled_;
        options.camera = owner.cameraState_;
        options.entityHelpers = false;
        options.detailProps = owner.detailPropsVisible_;
        options.animationSeconds = animationTimer_.isValid()
            ? animationTimer_.elapsed() / 1000.0 : 0.0;
        options.maximumAtlasSize = static_cast<int>(std::min<std::uint32_t>(
            8192u, deviceProperties_.limits.maxImageDimension2D));
        options.maximumAtlasLayers = static_cast<int>(
            deviceProperties_.limits.maxImageArrayLayers);
        hammer::render::RayTracingSceneBuilder builder(owner.materials_, owner.studioModels_);
        scene_ = owner.scene_ ? builder.build(*owner.scene_, options)
                              : hammer::render::RayTracingScene{};
        if (!scene_.valid() || !uploadScene(error)) {
            if (error.isEmpty()) {
                error = QString::fromStdString(scene_.error.empty()
                    ? std::string("The map contains no ray-traceable geometry") : scene_.error);
            }
            scene_ = {};
            return false;
        }

        // Every face of every probe shares one exposure. Letting the histogram
        // adapt per face would meter a bright face and a dark face differently
        // and leave visible steps at the cube seams.
        const float bakeToneScale =
            std::clamp(static_cast<float>(scene_.toneMap.scale), 0.001f, 16.0f);

        // Faces are stored in hammer::assets::CubeImage's own order - right
        // (+X), left (-X), back (+Y), front (-Y), up (+Z), down (-Z) - so a bake
        // is indistinguishable from a VTF cubemap and reaches the GL cube
        // through the same face reordering an authored $envmap already uses.
        //
        // Each {forward, right, up} basis inverts sampleHammerSkyDirection's
        // direction-to-face mapping in Hardware3DViewport, so a baked face lands
        // back on the pixels it was rendered from. `right` is the direction of
        // increasing column and `up` of decreasing row, which is what the
        // compute shader's ray setup expects after it flips screen.y.
        static constexpr std::array<std::array<std::array<float, 3>, 3>, 6> faceBases{{
            {{{1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}},
            {{{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}},
            {{{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}},
            {{{0.0f, -1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}},
            {{{0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}}},
            {{{0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}}}};

        output.assign(probes.size(), hammer::assets::CubeImage{});
        bool bakedAny = false;
        for (std::size_t probeIndex = 0; probeIndex < probes.size(); ++probeIndex) {
            const hammer::render::CubemapProbe& probe = probes[probeIndex];
            const int faceSize = std::clamp(probe.size, 1, 256);
            // Small authored sizes carry very few ray samples per face, so render
            // at a floor and box-filter down to the authored resolution. The
            // stored cubemap is still exactly the size the entity asked for.
            const int renderSize = std::max(faceSize, 64);
            bool probeFailed = false;
            for (std::size_t face = 0; face < 6 && !probeFailed; ++face) {
                CameraOverride cameraOverride;
                cameraOverride.position = {static_cast<float>(probe.origin.x),
                                           static_cast<float>(probe.origin.y),
                                           static_cast<float>(probe.origin.z)};
                cameraOverride.forward = faceBases[face][0];
                cameraOverride.right = faceBases[face][1];
                cameraOverride.up = faceBases[face][2];
                cameraOverride.toneScale = bakeToneScale;
                cameraOverride.suppressBloom = true;

                if (outputWidth_ != renderSize || outputHeight_ != renderSize) {
                    if (!createOutput(renderSize, renderSize, error)) return false;
                }
                if (!descriptorSet_ && !updateDescriptors(error)) return false;
                updateCamera(owner, renderSize, renderSize, false, &cameraOverride);

                QImage frame;
                if (!dispatchFrame(renderSize, renderSize, frame, error)) {
                    probeFailed = true;
                    break;
                }
                ++frameIndex_;
                // Each face is a single jittered sample. The downscale below
                // filters the small sizes, but a 128/256 probe is stored at the
                // render resolution and would keep its raw sampling noise.
                edgeAwareDenoise(frame);
                if (faceSize != renderSize) {
                    frame = frame.scaled(faceSize, faceSize, Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
                }
                output[probeIndex].faces[face] = imageFromFrame(frame);
            }
            if (!probeFailed) bakedAny = true;
        }

        // The bake scene has no entity helpers, so it must not be left behind as
        // the viewport's cache.
        scene_ = {};
        historyFrame_ = QImage{};
        hasCameraSignature_ = false;
        if (!bakedAny && error.isEmpty())
            error = QStringLiteral("No env_cubemap probe could be rendered");
        return bakedAny;
    }

    // QImage RGBA8888 -> the ARGB32 (0xAARRGGBB) layout assets::Image stores.
    static hammer::assets::Image imageFromFrame(const QImage& frame)
    {
        hammer::assets::Image image;
        if (frame.isNull()) return image;
        const QImage source = frame.convertToFormat(QImage::Format_RGBA8888);
        image.width = source.width();
        image.height = source.height();
        image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
        for (int row = 0; row < image.height; ++row) {
            const uchar* line = source.constScanLine(row);
            for (int column = 0; column < image.width; ++column) {
                const uchar* pixel = line + static_cast<std::size_t>(column) * 4;
                image.pixels[static_cast<std::size_t>(row) * image.width + column] =
                    (static_cast<std::uint32_t>(pixel[3]) << 24) |
                    (static_cast<std::uint32_t>(pixel[0]) << 16) |
                    (static_cast<std::uint32_t>(pixel[1]) << 8) |
                    static_cast<std::uint32_t>(pixel[2]);
            }
        }
        return image;
    }

private:
    // A cubemap face is rendered from an arbitrary orthonormal basis at a fixed
    // 90 degree square frustum, which yaw/pitch alone cannot express as cleanly.
    // Overriding the basis outright also lets the bake pin exposure, so the six
    // faces cannot meter themselves independently and seam at the edges.
    struct CameraOverride
    {
        std::array<float, 3> position{};
        std::array<float, 3> forward{};
        std::array<float, 3> right{};
        std::array<float, 3> up{};
        float toneScale{0.0f};
        // A cubemap face is scene data, not a displayed frame. Bloom is a
        // display effect, so baking it into a face would light the map with
        // its own glow every time a surface reflected that cubemap.
        bool suppressBloom{false};
    };

    void updateCamera(MapViewWidget& owner, int width, int height, bool interactive,
                      const CameraOverride* cameraOverride = nullptr)
    {
        CameraGpu camera{};
        const auto forward = hammer::camera::forwardVector(owner.cameraState_);
        const auto right = hammer::camera::rightVector(owner.cameraState_);
        const auto up = hammer::camera::upVector(owner.cameraState_);
        camera.cameraPosition = {static_cast<float>(owner.cameraState_.position.x),
                                 static_cast<float>(owner.cameraState_.position.y),
                                 static_cast<float>(owner.cameraState_.position.z), 1.0f};
        camera.cameraForwardTanHalfFov = {
            static_cast<float>(forward.x), static_cast<float>(forward.y),
            static_cast<float>(forward.z),
            static_cast<float>(std::tan(owner.cameraState_.verticalFovRadians * 0.5))};
        camera.cameraRightAspect = {
            static_cast<float>(right.x), static_cast<float>(right.y), static_cast<float>(right.z),
            static_cast<float>(width) / std::max(1.0f, static_cast<float>(height))};
        camera.cameraUpNear = {static_cast<float>(up.x), static_cast<float>(up.y),
                               static_cast<float>(up.z),
                               static_cast<float>(owner.cameraState_.nearPlane)};
        if (cameraOverride) {
            // tan(45 degrees) = 1 with a square aspect gives the exact 90 degree
            // frustum a cube face requires.
            camera.cameraPosition = {cameraOverride->position[0], cameraOverride->position[1],
                                     cameraOverride->position[2], 1.0f};
            camera.cameraForwardTanHalfFov = {cameraOverride->forward[0],
                                              cameraOverride->forward[1],
                                              cameraOverride->forward[2], 1.0f};
            camera.cameraRightAspect = {cameraOverride->right[0], cameraOverride->right[1],
                                        cameraOverride->right[2], 1.0f};
            camera.cameraUpNear = {cameraOverride->up[0], cameraOverride->up[1],
                                   cameraOverride->up[2],
                                   static_cast<float>(owner.cameraState_.nearPlane)};
        }
        camera.sunDirectionIntensity = {0.42f, -0.36f, -0.83f, 1.25f};
        camera.sunColorTime = {1.0f, 0.96f, 0.88f,
                               animationTimer_.isValid() ? animationTimer_.elapsed() / 1000.0f : 0.0f};
        camera.effectIntensities = {owner.phongIntensity_, owner.specularIntensity_,
                                    owner.bumpMapIntensity_, 1.0f};
        camera.backgroundColor = {0.06f, 0.075f, 0.10f, 1.0f};
        std::uint32_t controls = ControlDenoise;
        if (owner.phongEnabled_) controls |= ControlPhong;
        if (owner.specularEnabled_) controls |= ControlSpecular;
        if (owner.bumpMapsEnabled_) controls |= ControlBump;
        if (owner.lightWarpEnabled_) controls |= ControlLightWarp;
        if (owner.selfIllumEnabled_) controls |= ControlSelfIllum;
        if (owner.rimLightEnabled_) controls |= ControlRimLight;
        if (interactive) controls |= ControlInteractive;
        camera.renderControls = {static_cast<std::uint32_t>(width),
                                 static_cast<std::uint32_t>(height), controls, frameIndex_};
        camera.skyRects = scene_.skyRects;

        // The histogram pass updates this scale after each completed frame,
        // matching Source's dynamic eye-adaptation behavior.  The current frame
        // uses the previous frame's scale, just like the engine's feedback loop.
        const float toneScale = (cameraOverride && cameraOverride->toneScale > 0.0f)
            ? cameraOverride->toneScale : currentToneScale_;

        struct ActiveCorrection { float weight; std::uint32_t index; };
        std::vector<ActiveCorrection> activeCorrections;
        activeCorrections.reserve(scene_.colorCorrections.size());
        const auto& position = owner.cameraState_.position;
        for (const auto& correction : scene_.colorCorrections) {
            if (!correction.enabled || correction.originWeight[3] <= 0.0f) continue;
            float weight = correction.originWeight[3];
            if (correction.maximum[3] > 0.5f) {
                const bool inside =
                    position.x >= correction.minimum[0] && position.x <= correction.maximum[0] &&
                    position.y >= correction.minimum[1] && position.y <= correction.maximum[1] &&
                    position.z >= correction.minimum[2] && position.z <= correction.maximum[2];
                if (!inside) weight = 0.0f;
            } else {
                const double dx = position.x - correction.originWeight[0];
                const double dy = position.y - correction.originWeight[1];
                const double dz = position.z - correction.originWeight[2];
                const float distance = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
                const float minimum = correction.minimum[0];
                const float maximum = correction.minimum[1];
                if (minimum != -1.0f && maximum != -1.0f &&
                    std::abs(maximum - minimum) > 0.0001f) {
                    const float falloff = std::clamp((distance - minimum) / (maximum - minimum),
                                                     0.0f, 1.0f);
                    weight *= 1.0f - falloff;
                }
            }
            if (weight > 0.0001f)
                activeCorrections.push_back({weight, correction.lutIndex});
        }
        std::sort(activeCorrections.begin(), activeCorrections.end(),
                  [](const ActiveCorrection& a, const ActiveCorrection& b) {
                      return a.weight > b.weight;
                  });
        if (activeCorrections.size() > 4) activeCorrections.resize(4);
        float totalCorrectionWeight = 0.0f;
        for (std::size_t index = 0; index < activeCorrections.size(); ++index) {
            camera.colorCorrectionWeights[index] = activeCorrections[index].weight;
            camera.colorCorrectionIndices[index] = activeCorrections[index].index;
            totalCorrectionWeight += activeCorrections[index].weight;
        }
        // Source blends the uncorrected framebuffer using a separate default
        // weight alongside up to four lookup volumes. Keep the sum at one when
        // overlapping map corrections exceed full strength.
        if (totalCorrectionWeight > 1.0f) {
            for (std::size_t index = 0; index < activeCorrections.size(); ++index)
                camera.colorCorrectionWeights[index] /= totalCorrectionWeight;
            totalCorrectionWeight = 1.0f;
        }
        // With HDR off there is no auto-exposure and no bloom: the frame is
        // converted straight to gamma, which is what a non-HDR compile shows.
        const bool hdr = owner.hdrEnabled_;
        const bool suppressBloom = (cameraOverride && cameraOverride->suppressBloom) || !hdr;
        const float bloomScale = suppressBloom
            ? 0.0f : static_cast<float>(scene_.toneMap.bloomScale);
        bloomEnabled_ = bloomScale > 0.0f;
        // A cubemap bake must not burn the user's display gamma into the face,
        // so it converts with the 2.2 the rest of the engine assumes.
        const float displayGamma = cameraOverride ? 2.2f : owner.rayTracedGamma_;
        camera.toneMapControls = {hdr ? toneScale : 1.0f, bloomScale,
                                  std::max(0.0f, 1.0f - totalCorrectionWeight),
                                  displayGamma};
        camera.radiosityControls = {
            static_cast<std::uint32_t>(std::max(0, scene_.radiosity.patchLayout.width)),
            static_cast<std::uint32_t>(std::max(0, scene_.radiosity.patchLayout.height)),
            radiosity_.ready ? 1u : 0u,
            radiosity_.ready && radiosity_.propTriangleCount > 0 ? 1u : 0u};
        void* mapped = nullptr;
        if (vkMapMemory(device_, cameraBuffer_.memory, 0, sizeof(camera), 0, &mapped) == VK_SUCCESS && mapped) {
            std::memcpy(mapped, &camera, sizeof(camera));
            vkUnmapMemory(device_, cameraBuffer_.memory);
        }
    }

    void destroyAcceleration(AccelerationStructure& acceleration)
    {
        if (acceleration.handle && destroyAccelerationStructure_)
            destroyAccelerationStructure_(device_, acceleration.handle, nullptr);
        destroyBuffer(acceleration.storage);
        acceleration = {};
    }

    void destroySceneResources()
    {
        destroyAcceleration(topLevel_);
        destroyAcceleration(bottomLevel_);
        destroyBuffer(instanceBuffer_);
        destroyBuffer(vertexBuffer_);
        destroyBuffer(indexBuffer_);
        destroyBuffer(triangleBuffer_);
        destroyBuffer(materialBuffer_);
        destroyBuffer(cameraBuffer_);
        destroyBuffer(lightBuffer_);
        destroyBuffer(colorCorrectionBuffer_);
        destroyBuffer(exposureHistogramBuffer_);
        destroyBuffer(updateScratchBuffer_);
        if (atlasSampler_) vkDestroySampler(device_, atlasSampler_, nullptr);
        atlasSampler_ = VK_NULL_HANDLE;
        destroyImage(atlasImage_, atlasMemory_, atlasView_);
        if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
        descriptorSet_ = VK_NULL_HANDLE;
    }

    void shutdown()
    {
        if (!device_ && !instance_) return;
        if (device_) vkDeviceWaitIdle(device_);
        destroySceneResources();
        destroyImage(outputImage_, outputMemory_, outputView_);
        destroyImage(hdrImage_, hdrMemory_, hdrView_);
        destroyImage(bloomImageA_, bloomMemoryA_, bloomViewA_);
        destroyImage(bloomImageB_, bloomMemoryB_, bloomViewB_);
        for (VkPipeline pipeline : {brightPipeline_, blurPipeline_, compositePipeline_})
            if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
        for (VkShaderModule module : {brightModule_, blurModule_, compositeModule_})
            if (module) vkDestroyShaderModule(device_, module, nullptr);
        destroyBuffer(readback_);
        destroyRadiosityBuffers();
        for (VkPipeline pipeline : {radiosity_.patchDirectPipeline,
                                    radiosity_.transfersPipeline,
                                    radiosity_.bouncePipeline,
                                    radiosity_.propVertexPipeline})
            if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
        for (VkShaderModule module : {radiosity_.patchDirectModule,
                                      radiosity_.transfersModule,
                                      radiosity_.bounceModule,
                                      radiosity_.propVertexModule})
            if (module) vkDestroyShaderModule(device_, module, nullptr);
        if (radiosity_.descriptorPool)
            vkDestroyDescriptorPool(device_, radiosity_.descriptorPool, nullptr);
        if (radiosity_.pipelineLayout)
            vkDestroyPipelineLayout(device_, radiosity_.pipelineLayout, nullptr);
        if (radiosity_.descriptorLayout)
            vkDestroyDescriptorSetLayout(device_, radiosity_.descriptorLayout, nullptr);
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (shaderModule_) vkDestroyShaderModule(device_, shaderModule_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        if (device_) vkDestroyDevice(device_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
        physicalDevice_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
    }

    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkPhysicalDeviceProperties deviceProperties_{};
    VkDevice device_{VK_NULL_HANDLE};
    std::uint32_t queueFamily_{0};
    VkQueue queue_{VK_NULL_HANDLE};
    VkCommandPool commandPool_{VK_NULL_HANDLE};
    VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
    VkFence fence_{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptorLayout_{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkShaderModule shaderModule_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};

    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure_{nullptr};
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure_{nullptr};
    PFN_vkGetAccelerationStructureBuildSizesKHR getAccelerationStructureBuildSizes_{nullptr};
    PFN_vkCmdBuildAccelerationStructuresKHR commandBuildAccelerationStructures_{nullptr};
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAccelerationStructureDeviceAddress_{nullptr};

    Buffer vertexBuffer_;
    Buffer indexBuffer_;
    Buffer triangleBuffer_;
    Buffer materialBuffer_;
    Buffer cameraBuffer_;
    Buffer lightBuffer_;
    Buffer colorCorrectionBuffer_;
    Buffer exposureHistogramBuffer_;
    Buffer instanceBuffer_;
    Buffer readback_;
    RadiosityResources radiosity_;
    // Reused for deferred billboard/animated-geometry BLAS refits so dynamic
    // geometry updates do not repeatedly allocate/free device-local scratch memory.
    Buffer updateScratchBuffer_;
    AccelerationStructure bottomLevel_;
    AccelerationStructure topLevel_;
    VkImage atlasImage_{VK_NULL_HANDLE};
    VkDeviceMemory atlasMemory_{VK_NULL_HANDLE};
    VkImageView atlasView_{VK_NULL_HANDLE};
    VkSampler atlasSampler_{VK_NULL_HANDLE};
    // Linear HDR frame, plus two quarter-resolution bloom buffers the
    // separable blur ping-pongs between.
    VkImage hdrImage_{VK_NULL_HANDLE};
    VkDeviceMemory hdrMemory_{VK_NULL_HANDLE};
    VkImageView hdrView_{VK_NULL_HANDLE};
    VkImage bloomImageA_{VK_NULL_HANDLE};
    VkDeviceMemory bloomMemoryA_{VK_NULL_HANDLE};
    VkImageView bloomViewA_{VK_NULL_HANDLE};
    VkImage bloomImageB_{VK_NULL_HANDLE};
    VkDeviceMemory bloomMemoryB_{VK_NULL_HANDLE};
    VkImageView bloomViewB_{VK_NULL_HANDLE};
    int bloomWidth_{0};
    int bloomHeight_{0};
    VkShaderModule brightModule_{VK_NULL_HANDLE};
    VkShaderModule blurModule_{VK_NULL_HANDLE};
    VkShaderModule compositeModule_{VK_NULL_HANDLE};
    VkPipeline brightPipeline_{VK_NULL_HANDLE};
    VkPipeline blurPipeline_{VK_NULL_HANDLE};
    VkPipeline compositePipeline_{VK_NULL_HANDLE};
    VkImage outputImage_{VK_NULL_HANDLE};
    VkDeviceMemory outputMemory_{VK_NULL_HANDLE};
    VkImageView outputView_{VK_NULL_HANDLE};
    int outputWidth_{0};
    int outputHeight_{0};
    VkDeviceSize scratchAlignment_{256};
    hammer::render::RayTracingScene scene_;
    QElapsedTimer animationTimer_;
    std::array<float, 6> lastCameraSignature_{};
    bool hasCameraSignature_{false};
    QElapsedTimer cameraStableTimer_;
    QElapsedTimer toneMapTimer_;
    float currentToneScale_{1.0f};
    bool bloomEnabled_{false};
    bool spriteBillboardRefreshPending_{false};
    bool animationRefreshPending_{false};
    std::uint32_t frameIndex_{0};
    QImage historyFrame_;
    bool hardwareRayTracing_{false};
};

VulkanRayTracedViewport::VulkanRayTracedViewport(MapViewWidget* owner)
    : QWidget(owner), owner_(owner)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    animationTimer_ = new QTimer(this);
    animationTimer_->setTimerType(Qt::PreciseTimer);
    // Animated geometry currently requires an acceleration-structure refresh.
    // 10 Hz is smooth enough for an editor preview and avoids v0.15.14's
    // catastrophic full-scene rebuild at 30 Hz.
    animationTimer_->setInterval(100);
    connect(animationTimer_, &QTimer::timeout, this, [this] {
        if (!isVisible()) return;
        // Keep the GUI thread free while a modal dialog owns the interaction.
        if (renderingSuspended()) return;
        // A frame skipped while a dialog was up is picked up here, so the
        // preview catches up on its own once the dialog closes.
        if (frameDirty_) {
            update();
            return;
        }
        if (!renderer_) return;
        if (renderer_->hasAnimatedContent()) renderer_->requestAnimationRefresh();
        else if (!renderer_->hasPendingDeferredRefresh()) return;
        frameDirty_ = true;
        update();
    });
    animationTimer_->start();
}

VulkanRayTracedViewport::~VulkanRayTracedViewport()
{
    releaseRenderer();
}

void VulkanRayTracedViewport::invalidateMaterialCache()
{
    sceneDirty_ = true;
    requestUpdate(true);
}

void VulkanRayTracedViewport::invalidateGeometryCache()
{
    sceneDirty_ = true;
    requestUpdate(true);
}

void VulkanRayTracedViewport::requestUpdate(bool rerender)
{
    if (rerender) frameDirty_ = true;
    // Camera and material-control changes do not alter static geometry. Scene
    // rebuilds are requested only by invalidateGeometryCache(),
    // invalidateMaterialCache(), or the low-frequency animation refresh.
    update();
}

QString VulkanRayTracedViewport::rendererDescription() const
{
    if (renderer_) return renderer_->description();
    return rendererError_.isEmpty() ? QStringLiteral("Vulkan ray-traced preview") : rendererError_;
}

bool VulkanRayTracedViewport::hardwareRayTracingAvailable() const
{
    return renderer_ && renderer_->hardwareRayTracingAvailable();
}

bool VulkanRayTracedViewport::bakeCubemaps(
    const std::vector<hammer::render::CubemapProbe>& probes,
    std::vector<hammer::assets::CubeImage>& output, QString& error)
{
    if (!owner_) return false;
    if (!ensureRenderer()) {
        error = rendererError_.isEmpty()
            ? QStringLiteral("The ray-traced renderer is unavailable") : rendererError_;
        return false;
    }
    if (!renderer_->bakeCubemaps(*owner_, probes, output, error)) return false;
    // The bake dropped the renderer's scene cache, so the next paint of this
    // view has to rebuild the one that still carries entity helpers.
    sceneDirty_ = true;
    frameDirty_ = true;
    return true;
}

bool VulkanRayTracedViewport::ensureRenderer()
{
    if (renderer_) return true;
    auto renderer = std::make_unique<Renderer>();
    QString error;
    if (!renderer->initialize(error)) {
        qWarning("Ray-traced preview unavailable: %s", qPrintable(error));
        setRendererError(error);
        return false;
    }
    renderer_ = std::move(renderer);
    rendererError_.clear();
    return true;
}

bool VulkanRayTracedViewport::renderFrame()
{
    if (!owner_ || !ensureRenderer()) return false;
    QString error;
    if (!renderer_->render(*owner_, std::max(1, width()), std::max(1, height()),
                           sceneDirty_, frame_, error)) {
        setRendererError(error);
        return false;
    }
    sceneDirty_ = false;
    frameDirty_ = false;
    return true;
}

void VulkanRayTracedViewport::releaseRenderer()
{
    renderer_.reset();
}

void VulkanRayTracedViewport::setRendererError(const QString& error)
{
    rendererError_ = error;
    frameDirty_ = false;
    update();
}

// A ray-traced frame is produced synchronously on the GUI thread, and a scene
// carrying detail props is heavy enough that a steady stream of them starves
// everything else - which is what made the Open dialog take seconds to appear
// and then miss mouse input. While a modal dialog is up, the preview holds its
// last frame instead of computing new ones; there is nothing to look at behind
// a modal dialog anyway. The 10 Hz deferred-refresh timer honours the same
// rule, so a pending sprite refresh cannot sneak a full acceleration-structure
// refit in either.
//
// activeModalWidget() only ever sees Qt's own dialogs. The native file chooser
// - a GTK or xdg-desktop-portal window under GNOME - is not a QWidget, so the
// callers that open one hold a PreviewRenderSuspension to say so; see
// PreviewRenderGate.hpp.
bool VulkanRayTracedViewport::renderingSuspended()
{
    if (hammer::app::previewRenderingSuspended()) return true;
    const QWidget* modal = QApplication::activeModalWidget();
    return modal != nullptr;
}

void VulkanRayTracedViewport::paintEvent(QPaintEvent*)
{
    if (frameDirty_ && !renderingSuspended()) renderFrame();
    QPainter painter(this);
    painter.fillRect(rect(), QColor(12, 12, 12));
    if (!frame_.isNull()) painter.drawImage(rect(), frame_);
    if (renderer_ && rendererError_.isEmpty()) {
        painter.setPen(QColor(210, 225, 240, 210));
        painter.drawText(QRect(10, 8, width() - 20, 24),
                         Qt::AlignLeft | Qt::AlignTop,
                         renderer_->description() + QStringLiteral(" — 100% settled / 50% moving ray resolution"));
    }
    if (!rendererError_.isEmpty()) {
        painter.setPen(QColor(255, 184, 96));
        painter.drawText(rect().adjusted(18, 18, -18, -18),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         QStringLiteral("Ray-Traced Preview unavailable\n%1\n\n"
                                        "The normal OpenGL viewport remains active in the other modes.")
                             .arg(rendererError_));
    }
    if (owner_) owner_->paintHardwareOverlay(painter, true);
}

void VulkanRayTracedViewport::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    frameDirty_ = true;
}
