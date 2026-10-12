// =============================================================================
// Zakura - Vulkan 3D Null, Cube Projection & Vector Optical Smear Engine
// High-Throughput 3D Euclidean Rasterizer & Optical Flow Motion Blur (Adevob Style)
// Part 1 of 2: Architecture, Math Matrices, Context & Memory Allocation
// =============================================================================

#include <jni.h>
#include <android/log.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <memory>
#include <mutex>
#include <functional>
#include <array>

#define LOG_TAG "ZakuraVulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// =============================================================================
// 3D GEOMETRIC & OPTICAL FLOW MATH STRUCTURES
// =============================================================================

struct Vec2 {
    float x, y;
};

struct Vec3 {
    float x, y, z;
};

struct Vec4 {
    float x, y, z, w;
};

struct Mat4 {
    float m[16];

    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = 1.0f; r.m[5] = 1.0f; r.m[10] = 1.0f; r.m[15] = 1.0f;
        return r;
    }

    static Mat4 multiply(const Mat4& a, const Mat4& b) {
        Mat4 r{};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                r.m[i * 4 + j] = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
                }
            }
        }
        return r;
    }

    static Mat4 perspective(float fovRad, float aspect, float zNear, float zFar) {
        Mat4 r{};
        float tanHalfFov = std::tan(fovRad / 2.0f);
        r.m[0] = 1.0f / (aspect * tanHalfFov);
        r.m[5] = 1.0f / tanHalfFov;
        r.m[10] = -(zFar + zNear) / (zFar - zNear);
        r.m[11] = -1.0f;
        r.m[14] = -(2.0f * zFar * zNear) / (zFar - zNear);
        return r;
    }

    static Mat4 translation(float tx, float ty, float tz) {
        Mat4 r = identity();
        r.m[12] = tx;
        r.m[13] = ty;
        r.m[14] = tz;
        return r;
    }

    static Mat4 rotationY(float angleRad) {
        Mat4 r = identity();
        float c = std::cos(angleRad);
        float s = std::sin(angleRad);
        r.m[0] = c;  r.m[2] = -s;
        r.m[8] = s;  r.m[10] = c;
        return r;
    }

    static Mat4 rotationX(float angleRad) {
        Mat4 r = identity();
        float c = std::cos(angleRad);
        float s = std::sin(angleRad);
        r.m[5] = c;  r.m[6] = s;
        r.m[9] = -s; r.m[10] = c;
        return r;
    }

    static Mat4 rotationZ(float angleRad) {
        Mat4 r = identity();
        float c = std::cos(angleRad);
        float s = std::sin(angleRad);
        r.m[0] = c;  r.m[1] = s;
        r.m[4] = -s; r.m[5] = c;
        return r;
    }
};

// Push constants passed to GLSL compute passes
struct ZakuraPushConstants {
    int32_t passIndex;       // 0: 3D Raster/Cube, 1: Vector Optical Smear, 2: Edge Effects/Particles, 3: Final Composite
    int32_t targetWidth;
    int32_t targetHeight;
    int32_t bandY;           // Y offset for row-banded dispatching
    int32_t bandHeight;      // Number of rows in current dispatch band
    float shutterAngle;      // e.g. 180.0 to 720.0
    int32_t smearSamples;    // e.g. 6 to 16 samples for stepped trailing contours
    float blurIntensity;     // 0.0 to 2.0
    float timeSeconds;
    int32_t edgeEffectMode;  // 0: none, 1: neon scratch, 2: halftone, 3: boundary particles
    float edgeGlowStrength;
    int32_t seed;
};

// Uniform buffer structure layout (up to 4096 floats)
// 0..63: Camera & 3D Null transform matrices
// 64..255: Face quad vertices and UV mapping
// 256..511: Motion vector deltas for Optical Flow Smear
// 512..1023: Dynamic 3D Particle state table
struct ZakuraUniformLayout {
    float cameraViewProj[16];
    float nullTransform[16];
    float prevNullTransform[16]; // For frame-to-frame velocity tracking
    float faceRotations[6];      // For unfolding cross-box cube faces
    float padding1[10];

    // 6 Cube Faces (Front, Back, Left, Right, Top, Bottom)
    // Each face has 4 vertices (Vec4 x,y,z,w)
    float faceVertices[6 * 4 * 4];

    // Optical Flow Global Deltas
    float velocityDeltaX;
    float velocityDeltaY;
    float velocityAngle;
    float velocityMagnitude;
    float padding2[12];

    // Surrounding 3D Floating Particles (up to 128 particles: pos.xyz, size, color.rgba)
    float particles[128 * 8];
};

// =============================================================================
// VULKAN RUNTIME CONTEXT
// =============================================================================

struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    uint32_t computeQueueFamilyIndex = 0;

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline computePipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence computeFence = VK_NULL_HANDLE;

    // Hardware Samplers (Linear and Point Clamp)
    VkSampler linearSampler = VK_NULL_HANDLE;
    VkSampler nearestSampler = VK_NULL_HANDLE;

    // Buffers for 3D Cards, Frame Data & Motion Vectors
    VkBuffer inCurrentFrameBuffer = VK_NULL_HANDLE;
    VkDeviceMemory inCurrentFrameMemory = VK_NULL_HANDLE;

    VkBuffer inPrevFrameBuffer = VK_NULL_HANDLE;
    VkDeviceMemory inPrevFrameMemory = VK_NULL_HANDLE;

    VkBuffer outBuffer = VK_NULL_HANDLE;
    VkDeviceMemory outMemory = VK_NULL_HANDLE;

    VkBuffer uboBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uboMemory = VK_NULL_HANDLE;

    // Optical Flow Vector Field Buffer (FP16 velocity map per pixel: 4 bytes per texel)
    VkBuffer velocityMapBuffer = VK_NULL_HANDLE;
    VkDeviceMemory velocityMapMemory = VK_NULL_HANDLE;

    // 3D Scene Geometry Intermediate Buffer
    VkBuffer sceneRasterBuffer = VK_NULL_HANDLE;
    VkDeviceMemory sceneRasterMemory = VK_NULL_HANDLE;

    // Edge FX and Particle Accumulator Buffer
    VkBuffer particleFxBuffer = VK_NULL_HANDLE;
    VkDeviceMemory particleFxMemory = VK_NULL_HANDLE;

    size_t allocatedPixelCapacity = 0;
    bool isInitialized = false;
    std::mutex pipelineMutex;
};

static VulkanContext gVk;
static int32_t gLastRenderStatus = 0; // 0: OK, 1: GPU Fallback, 2: Device Lost, 3: Timeout

constexpr uint64_t kFenceTimeoutNs = 30ull * 1000000000ull;
constexpr int32_t  kMaxPixelsPerSubmit = 500000; // Safe watchdog chunk for mobile Mali/Adreno GPUs

// =============================================================================
// MEMORY ALLOCATION HELPERS
// =============================================================================

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    if (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
        return findMemoryType(physicalDevice, typeFilter, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }

    LOGE("Failed to find suitable Vulkan memory type!");
    return 0;
}

void createBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size,
                  VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                  VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        LOGE("Failed to create Vulkan buffer: %llu bytes", (unsigned long long)size);
        return;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, buffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReqs.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        LOGE("Failed to allocate Vulkan memory: %llu bytes", (unsigned long long)memReqs.size);
        return;
    }

    vkBindBufferMemory(device, buffer, bufferMemory, 0);
}

void createHardwareSamplers() {
    VkSamplerCreateInfo linearInfo{};
    linearInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    linearInfo.magFilter = VK_FILTER_LINEAR;
    linearInfo.minFilter = VK_FILTER_LINEAR;
    linearInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    linearInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    linearInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    linearInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(gVk.device, &linearInfo, nullptr, &gVk.linearSampler) != VK_SUCCESS) {
        LOGE("Failed to create Vulkan Linear Sampler.");
    }

    VkSamplerCreateInfo nearestInfo{};
    nearestInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    nearestInfo.magFilter = VK_FILTER_NEAREST;
    nearestInfo.minFilter = VK_FILTER_NEAREST;
    nearestInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    nearestInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    nearestInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    nearestInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(gVk.device, &nearestInfo, nullptr, &gVk.nearestSampler) != VK_SUCCESS) {
        LOGE("Failed to create Vulkan Nearest Sampler.");
    }
}

void cleanupBuffers() {
    if (gVk.device == VK_NULL_HANDLE) return;

    auto safeDestroy = [](VkBuffer& buf, VkDeviceMemory& mem) {
        if (buf != VK_NULL_HANDLE) {
            vkDestroyBuffer(gVk.device, buf, nullptr);
            buf = VK_NULL_HANDLE;
        }
        if (mem != VK_NULL_HANDLE) {
            vkFreeMemory(gVk.device, mem, nullptr);
            mem = VK_NULL_HANDLE;
        }
    };

    safeDestroy(gVk.inCurrentFrameBuffer, gVk.inCurrentFrameMemory);
    safeDestroy(gVk.inPrevFrameBuffer, gVk.inPrevFrameMemory);
    safeDestroy(gVk.outBuffer, gVk.outMemory);
    safeDestroy(gVk.uboBuffer, gVk.uboMemory);
    safeDestroy(gVk.velocityMapBuffer, gVk.velocityMapMemory);
    safeDestroy(gVk.sceneRasterBuffer, gVk.sceneRasterMemory);
    safeDestroy(gVk.particleFxBuffer, gVk.particleFxMemory);

    gVk.allocatedPixelCapacity = 0;
}

bool ensureBuffersCapacity(size_t requiredPixels) {
    if (gVk.allocatedPixelCapacity >= requiredPixels && gVk.inCurrentFrameBuffer != VK_NULL_HANDLE) {
        return true;
    }

    cleanupBuffers();

    VkDeviceSize pixelBufferSize = requiredPixels * 4 * sizeof(uint8_t); // RGBA8
    VkDeviceSize uboBufferSize = 4096 * sizeof(float);
    VkDeviceSize velocityBufferSize = requiredPixels * 2 * sizeof(uint16_t); // FP16 2D motion vector map
    VkDeviceSize intermediateSceneSize = requiredPixels * 4 * sizeof(uint8_t);

    createBuffer(gVk.device, gVk.physicalDevice, pixelBufferSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 gVk.inCurrentFrameBuffer, gVk.inCurrentFrameMemory);

    createBuffer(gVk.device, gVk.physicalDevice, pixelBufferSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 gVk.inPrevFrameBuffer, gVk.inPrevFrameMemory);

    createBuffer(gVk.device, gVk.physicalDevice, pixelBufferSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 gVk.outBuffer, gVk.outMemory);

    createBuffer(gVk.device, gVk.physicalDevice, uboBufferSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 gVk.uboBuffer, gVk.uboMemory);

    createBuffer(gVk.device, gVk.physicalDevice, velocityBufferSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 gVk.velocityMapBuffer, gVk.velocityMapMemory);

    createBuffer(gVk.device, gVk.physicalDevice, intermediateSceneSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 gVk.sceneRasterBuffer, gVk.sceneRasterMemory);

    createBuffer(gVk.device, gVk.physicalDevice, intermediateSceneSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 gVk.particleFxBuffer, gVk.particleFxMemory);

    VkDescriptorBufferInfo inCurrInfo{gVk.inCurrentFrameBuffer, 0, pixelBufferSize};
    VkDescriptorBufferInfo inPrevInfo{gVk.inPrevFrameBuffer, 0, pixelBufferSize};
    VkDescriptorBufferInfo outInfo{gVk.outBuffer, 0, pixelBufferSize};
    VkDescriptorBufferInfo uboInfo{gVk.uboBuffer, 0, uboBufferSize};
    VkDescriptorBufferInfo velInfo{gVk.velocityMapBuffer, 0, velocityBufferSize};
    VkDescriptorBufferInfo sceneInfo{gVk.sceneRasterBuffer, 0, intermediateSceneSize};
    VkDescriptorBufferInfo partInfo{gVk.particleFxBuffer, 0, intermediateSceneSize};

    VkWriteDescriptorSet writes[7]{};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = gVk.descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &inCurrInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = gVk.descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &inPrevInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = gVk.descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo = &outInfo;

    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = gVk.descriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].descriptorCount = 1;
    writes[3].pBufferInfo = &uboInfo;

    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = gVk.descriptorSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].descriptorCount = 1;
    writes[4].pBufferInfo = &velInfo;

    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = gVk.descriptorSet;
    writes[5].dstBinding = 5;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].descriptorCount = 1;
    writes[5].pBufferInfo = &sceneInfo;

    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = gVk.descriptorSet;
    writes[6].dstBinding = 6;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[6].descriptorCount = 1;
    writes[6].pBufferInfo = &partInfo;

    vkUpdateDescriptorSets(gVk.device, 7, writes, 0, nullptr);

    gVk.allocatedPixelCapacity = requiredPixels;
    LOGI("Allocated Zakura 3D & Optical Smear GPU buffers for %zu pixels.", requiredPixels);
    return true;
}
// =============================================================================
// Zakura - Vulkan 3D Null, Cube Projection & Vector Optical Smear Engine
// Part 2 of 2: GPU Pipeline Execution, CPU Fallback & JNI Bridge Exports
// =============================================================================

// =============================================================================
// CPU FALLBACK RASTERIZER & OPTICAL SMEAR (Emergency / Emulator Path)
// =============================================================================

static inline float clampF(float v, float minV, float maxV) {
    return std::max(minV, std::min(maxV, v));
}

void executeCpuFallbackZakura(const uint32_t* currFrame, const uint32_t* prevFrame,
                              uint32_t* dst, int w, int h, const float* ubo,
                              const ZakuraPushConstants& pc) {
    if (!currFrame || !dst || w <= 0 || h <= 0) return;

    float vx = ubo ? ubo[64 + 0] : 0.0f; // Velocity X delta
    float vy = ubo ? ubo[64 + 1] : 0.0f; // Velocity Y delta
    float intensity = pc.blurIntensity;
    int samples = std::max(1, std::min(pc.smearSamples, 16));

    #pragma omp parallel for collapse(2)
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = y * w + x;
            uint32_t basePixel = currFrame[idx];

            if (intensity <= 0.01f || (std::abs(vx) < 0.1f && std::abs(vy) < 0.1f)) {
                dst[idx] = basePixel;
                continue;
            }

            // Stepped vector accumulation along optical delta
            float accumR = 0.0f, accumG = 0.0f, accumB = 0.0f, accumA = 0.0f;
            float totalWeight = 0.0f;

            for (int s = 0; s < samples; ++s) {
                float t = static_cast<float>(s) / static_cast<float>(samples);
                float sampleX = static_cast<float>(x) - vx * t * intensity;
                float sampleY = static_cast<float>(y) - vy * t * intensity;

                int ix = std::clamp(static_cast<int>(std::round(sampleX)), 0, w - 1);
                int iy = std::clamp(static_cast<int>(std::round(sampleY)), 0, h - 1);

                uint32_t sp = currFrame[iy * w + ix];
                float weight = 1.0f - (t * 0.45f); // Soft linear shutter trail falloff

                accumR += (sp & 0xFF) * weight;
                accumG += ((sp >> 8) & 0xFF) * weight;
                accumB += ((sp >> 16) & 0xFF) * weight;
                accumA += ((sp >> 24) & 0xFF) * weight;
                totalWeight += weight;
            }

            uint32_t ur = static_cast<uint32_t>(clampF(accumR / totalWeight, 0.0f, 255.0f));
            uint32_t ug = static_cast<uint32_t>(clampF(accumG / totalWeight, 0.0f, 255.0f));
            uint32_t ub = static_cast<uint32_t>(clampF(accumB / totalWeight, 0.0f, 255.0f));
            uint32_t ua = static_cast<uint32_t>(clampF(accumA / totalWeight, 0.0f, 255.0f));

            dst[idx] = (ua << 24) | (ub << 16) | (ug << 8) | ur;
        }
    }
}

// =============================================================================
// ROBUST, ROW-BANDED VULKAN GPU PIPELINE
// Prevents Android GPU Watchdog Timeout / Fence Hang on 1080p, 2K & 4K frames
// =============================================================================

static bool submitAndWait(const std::function<void()>& record) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkResetCommandBuffer(gVk.commandBuffer, 0);
    if (vkBeginCommandBuffer(gVk.commandBuffer, &beginInfo) != VK_SUCCESS) {
        LOGE("vkBeginCommandBuffer failed in Zakura pipeline.");
        return false;
    }

    vkCmdBindPipeline(gVk.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, gVk.computePipeline);
    vkCmdBindDescriptorSets(gVk.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            gVk.pipelineLayout, 0, 1, &gVk.descriptorSet, 0, nullptr);

    record();

    if (vkEndCommandBuffer(gVk.commandBuffer) != VK_SUCCESS) {
        LOGE("vkEndCommandBuffer failed in Zakura pipeline.");
        return false;
    }

    vkResetFences(gVk.device, 1, &gVk.computeFence);
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &gVk.commandBuffer;

    VkResult sr = vkQueueSubmit(gVk.computeQueue, 1, &submitInfo, gVk.computeFence);
    if (sr != VK_SUCCESS) {
        LOGE("vkQueueSubmit failed in Zakura pipeline: %d", (int)sr);
        gLastRenderStatus = (sr == VK_ERROR_DEVICE_LOST) ? 2 : 1;
        return false;
    }

    VkResult wr = vkWaitForFences(gVk.device, 1, &gVk.computeFence, VK_TRUE, kFenceTimeoutNs);
    if (wr != VK_SUCCESS) {
        LOGE("vkWaitForFences timed out in Zakura pipeline (status: %d)", (int)wr);
        gLastRenderStatus = (wr == VK_ERROR_DEVICE_LOST) ? 2 : 3;
        return false;
    }
    return true;
}

static void recordBarrier() {
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(gVk.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

static void recordPass(int32_t pass, int32_t w, int32_t h, int32_t bandY, int32_t bandRows,
                       float shutterAngle, int32_t smearSamples, float blurIntensity,
                       float timeSec, int32_t edgeMode, float edgeStrength, int32_t seed) {
    ZakuraPushConstants pc{};
    pc.passIndex = pass;
    pc.targetWidth = w;
    pc.targetHeight = h;
    pc.bandY = bandY;
    pc.bandHeight = bandRows;
    pc.shutterAngle = shutterAngle;
    pc.smearSamples = smearSamples;
    pc.blurIntensity = blurIntensity;
    pc.timeSeconds = timeSec;
    pc.edgeEffectMode = edgeMode;
    pc.edgeGlowStrength = edgeStrength;
    pc.seed = seed;

    vkCmdPushConstants(gVk.commandBuffer, gVk.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(ZakuraPushConstants), &pc);

    // 16x16 compute tile dispatch
    vkCmdDispatch(gVk.commandBuffer, (w + 15) / 16, (bandRows + 15) / 16, 1);
}

static bool runZakuraGpuPipelineOnce(int32_t width, int32_t height, int32_t pixelsPerSubmit,
                                     float shutterAngle, int32_t smearSamples, float blurIntensity,
                                     float timeSec, int32_t edgeMode, float edgeStrength, int32_t seed) {
    const int32_t bandRows = std::max<int32_t>(16, ((pixelsPerSubmit / std::max(width, 1)) / 16) * 16);

    // 0. Clean intermediate GPU scratch buffers
    if (!submitAndWait([&]() {
            VkBuffer clearList[] = {gVk.sceneRasterBuffer, gVk.velocityMapBuffer, gVk.particleFxBuffer, gVk.outBuffer};
            for (VkBuffer b : clearList) {
                vkCmdFillBuffer(gVk.commandBuffer, b, 0, VK_WHOLE_SIZE, 0);
            }
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(gVk.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        })) return false;

    // 1. Pass 0: 3D Scene Geometry & 3D Null Cube Projection
    for (int32_t y = 0; y < height; y += bandRows) {
        int32_t rows = std::min(bandRows, height - y);
        if (!submitAndWait([&]() {
                recordPass(0, width, height, y, rows, shutterAngle, smearSamples, blurIntensity, timeSec, edgeMode, edgeStrength, seed);
            })) return false;
    }

    // 2. Pass 1: Vector Optical Flow Calculation (Inter-frame deltas)
    if (!submitAndWait([&]() {
            recordPass(1, width, height, 0, height, shutterAngle, smearSamples, blurIntensity, timeSec, edgeMode, edgeStrength, seed);
            recordBarrier();
        })) return false;

    // 3. Pass 2: Adevob Optical Vector Smear (Streaked sub-frame contour trails)
    for (int32_t y = 0; y < height; y += bandRows) {
        int32_t rows = std::min(bandRows, height - y);
        if (!submitAndWait([&]() {
                recordPass(2, width, height, y, rows, shutterAngle, smearSamples, blurIntensity, timeSec, edgeMode, edgeStrength, seed);
            })) return false;
    }

    // 4. Pass 3: Edge Glow / Halftone Boundaries & 3D Surrounding Particles
    if (!submitAndWait([&]() {
            recordPass(3, width, height, 0, height, shutterAngle, smearSamples, blurIntensity, timeSec, edgeMode, edgeStrength, seed);
            recordBarrier();
        })) return false;

    // 5. Pass 4: Final Frame Composite to Output Buffer
    for (int32_t y = 0; y < height; y += bandRows) {
        int32_t rows = std::min(bandRows, height - y);
        if (!submitAndWait([&]() {
                recordPass(4, width, height, y, rows, shutterAngle, smearSamples, blurIntensity, timeSec, edgeMode, edgeStrength, seed);
            })) return false;
    }

    return true;
}

static bool runZakuraGpuPipeline(int32_t width, int32_t height,
                                 float shutterAngle, int32_t smearSamples, float blurIntensity,
                                 float timeSec, int32_t edgeMode, float edgeStrength, int32_t seed) {
    gLastRenderStatus = 0;
    int32_t budget = kMaxPixelsPerSubmit;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (runZakuraGpuPipelineOnce(width, height, budget, shutterAngle, smearSamples, blurIntensity, timeSec, edgeMode, edgeStrength, seed)) {
            return true;
        }
        if (gLastRenderStatus == 2 || gLastRenderStatus == 3) break; // Device lost / hung
        budget = std::max(65536, budget / 2);
        LOGE("Retrying Zakura GPU pipeline with smaller bands (%d px/submit)...", budget);
    }

    if (gLastRenderStatus == 0) gLastRenderStatus = 1;
    LOGE("Zakura GPU render failed (Status: %d) for resolution %dx%d.", gLastRenderStatus, width, height);
    return false;
}

} // namespace

// =============================================================================
// C EXPORTS FOR DART FFI
// =============================================================================

extern "C" {

int32_t get_last_render_status() {
    return gLastRenderStatus;
}

int32_t init_vulkan(const uint8_t* shaderBytes, int32_t length, int32_t precisionMode) {
    std::lock_guard<std::mutex> lock(gVk.pipelineMutex);
    if (gVk.isInitialized) return 1;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "ZakuraEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "ZakuraVulkan3D";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&createInfo, nullptr, &gVk.instance) != VK_SUCCESS) {
        LOGE("Failed to create Vulkan instance for Zakura.");
        return 0;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(gVk.instance, &deviceCount, nullptr);
    if (deviceCount == 0) return 0;

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(gVk.instance, &deviceCount, devices.data());
    gVk.physicalDevice = devices[0];

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gVk.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gVk.physicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            gVk.computeQueueFamilyIndex = i;
            break;
        }
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = gVk.computeQueueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceFeatures deviceFeatures{};
    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.pEnabledFeatures = &deviceFeatures;

    if (vkCreateDevice(gVk.physicalDevice, &deviceCreateInfo, nullptr, &gVk.device) != VK_SUCCESS) {
        LOGE("Failed to create Vulkan logical device for Zakura.");
        return 0;
    }

    vkGetDeviceQueue(gVk.device, gVk.computeQueueFamilyIndex, 0, &gVk.computeQueue);

    createHardwareSamplers();

    VkShaderModuleCreateInfo shaderModuleInfo{};
    shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleInfo.codeSize = length;
    shaderModuleInfo.pCode = reinterpret_cast<const uint32_t*>(shaderBytes);

    if (vkCreateShaderModule(gVk.device, &shaderModuleInfo, nullptr, &gVk.shaderModule) != VK_SUCCESS) {
        LOGE("Failed to create Zakura shader module.");
        return 0;
    }

    VkDescriptorSetLayoutBinding bindings[7]{};
    for (int b = 0; b < 7; b++) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 7;
    layoutInfo.pBindings = bindings;
    vkCreateDescriptorSetLayout(gVk.device, &layoutInfo, nullptr, &gVk.descriptorSetLayout);

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(ZakuraPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &gVk.descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    vkCreatePipelineLayout(gVk.device, &pipelineLayoutInfo, nullptr, &gVk.pipelineLayout);

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.layout = gVk.pipelineLayout;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = gVk.shaderModule;
    pipelineInfo.stage.pName = "main";
    vkCreateComputePipelines(gVk.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gVk.computePipeline);

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 7;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;
    vkCreateDescriptorPool(gVk.device, &poolInfo, nullptr, &gVk.descriptorPool);

    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = gVk.descriptorPool;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &gVk.descriptorSetLayout;
    vkAllocateDescriptorSets(gVk.device, &setAllocInfo, &gVk.descriptorSet);

    VkCommandPoolCreateInfo cmdPoolInfo{};
    cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.queueFamilyIndex = gVk.computeQueueFamilyIndex;
    cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(gVk.device, &cmdPoolInfo, nullptr, &gVk.commandPool);

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = gVk.commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(gVk.device, &cmdAllocInfo, &gVk.commandBuffer);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = 0;
    vkCreateFence(gVk.device, &fenceInfo, nullptr, &gVk.computeFence);

    gVk.isInitialized = true;
    LOGI("Zakura Vulkan Engine initialized successfully.");
    return 1;
}

void process_image(const uint8_t* currBytes, const uint8_t* prevBytes,
                   int32_t inW, int32_t inH,
                   uint8_t* outBytes, int32_t outW, int32_t outH,
                   const float* uniforms, int32_t uniformCount,
                   float shutterAngle, int32_t smearSamples, float blurIntensity,
                   float timeSeconds, int32_t edgeEffectMode, float edgeGlowStrength, int32_t seed) {
    std::lock_guard<std::mutex> lock(gVk.pipelineMutex);

    size_t pixelCount = static_cast<size_t>(outW * outH);
    size_t byteSize = pixelCount * 4 * sizeof(uint8_t);

    ZakuraPushConstants pc{};
    pc.targetWidth = outW;
    pc.targetHeight = outH;
    pc.shutterAngle = shutterAngle;
    pc.smearSamples = smearSamples;
    pc.blurIntensity = blurIntensity;
    pc.timeSeconds = timeSeconds;
    pc.edgeEffectMode = edgeEffectMode;
    pc.edgeGlowStrength = edgeGlowStrength;
    pc.seed = seed;

    if (!gVk.isInitialized || !ensureBuffersCapacity(pixelCount)) {
        executeCpuFallbackZakura(reinterpret_cast<const uint32_t*>(currBytes),
                                 reinterpret_cast<const uint32_t*>(prevBytes),
                                 reinterpret_cast<uint32_t*>(outBytes),
                                 outW, outH, uniforms, pc);
        return;
    }

    // 1. Upload Current Frame Pixels
    void* mappedCurr = nullptr;
    vkMapMemory(gVk.device, gVk.inCurrentFrameMemory, 0, byteSize, 0, &mappedCurr);
    std::memcpy(mappedCurr, currBytes, byteSize);
    VkMappedMemoryRange currRange{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, gVk.inCurrentFrameMemory, 0, VK_WHOLE_SIZE};
    vkFlushMappedMemoryRanges(gVk.device, 1, &currRange);
    vkUnmapMemory(gVk.device, gVk.inCurrentFrameMemory);

    // 2. Upload Previous Frame Pixels (if available for optical flow)
    void* mappedPrev = nullptr;
    vkMapMemory(gVk.device, gVk.inPrevFrameMemory, 0, byteSize, 0, &mappedPrev);
    if (prevBytes) {
        std::memcpy(mappedPrev, prevBytes, byteSize);
    } else {
        std::memcpy(mappedPrev, currBytes, byteSize);
    }
    VkMappedMemoryRange prevRange{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, gVk.inPrevFrameMemory, 0, VK_WHOLE_SIZE};
    vkFlushMappedMemoryRanges(gVk.device, 1, &prevRange);
    vkUnmapMemory(gVk.device, gVk.inPrevFrameMemory);

    // 3. Upload 3D Scene & Motion Uniforms
    void* mappedUbo = nullptr;
    size_t uboCopyBytes = std::min(size_t(uniformCount * sizeof(float)), size_t(4096 * sizeof(float)));
    vkMapMemory(gVk.device, gVk.uboMemory, 0, uboCopyBytes, 0, &mappedUbo);
    std::memcpy(mappedUbo, uniforms, uboCopyBytes);
    VkMappedMemoryRange uboRange{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, gVk.uboMemory, 0, VK_WHOLE_SIZE};
    vkFlushMappedMemoryRanges(gVk.device, 1, &uboRange);
    vkUnmapMemory(gVk.device, gVk.uboMemory);

    // 4. Run Banded Multi-Pass GPU Pipeline
    if (!runZakuraGpuPipeline(outW, outH, shutterAngle, smearSamples, blurIntensity, timeSeconds, edgeEffectMode, edgeGlowStrength, seed)) {
        executeCpuFallbackZakura(reinterpret_cast<const uint32_t*>(currBytes),
                                 reinterpret_cast<const uint32_t*>(prevBytes),
                                 reinterpret_cast<uint32_t*>(outBytes),
                                 outW, outH, uniforms, pc);
        return;
    }

    // 5. Read Back Rendered Pixels
    void* mappedOut = nullptr;
    vkMapMemory(gVk.device, gVk.outMemory, 0, byteSize, 0, &mappedOut);
    VkMappedMemoryRange outRange{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, gVk.outMemory, 0, VK_WHOLE_SIZE};
    vkInvalidateMappedMemoryRanges(gVk.device, 1, &outRange);
    std::memcpy(outBytes, mappedOut, byteSize);
    vkUnmapMemory(gVk.device, gVk.outMemory);
}

void cleanup_processor() {
    std::lock_guard<std::mutex> lock(gVk.pipelineMutex);
    if (!gVk.isInitialized) return;

    cleanupBuffers();

    if (gVk.linearSampler != VK_NULL_HANDLE) {
        vkDestroySampler(gVk.device, gVk.linearSampler, nullptr);
        gVk.linearSampler = VK_NULL_HANDLE;
    }
    if (gVk.nearestSampler != VK_NULL_HANDLE) {
        vkDestroySampler(gVk.device, gVk.nearestSampler, nullptr);
        gVk.nearestSampler = VK_NULL_HANDLE;
    }
    if (gVk.computeFence != VK_NULL_HANDLE) {
        vkDestroyFence(gVk.device, gVk.computeFence, nullptr);
        gVk.computeFence = VK_NULL_HANDLE;
    }
    if (gVk.commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(gVk.device, gVk.commandPool, nullptr);
        gVk.commandPool = VK_NULL_HANDLE;
    }
    if (gVk.descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(gVk.device, gVk.descriptorPool, nullptr);
        gVk.descriptorPool = VK_NULL_HANDLE;
    }
    if (gVk.computePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(gVk.device, gVk.computePipeline, nullptr);
        gVk.computePipeline = VK_NULL_HANDLE;
    }
    if (gVk.pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(gVk.device, gVk.pipelineLayout, nullptr);
        gVk.pipelineLayout = VK_NULL_HANDLE;
    }
    if (gVk.descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(gVk.device, gVk.descriptorSetLayout, nullptr);
        gVk.descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (gVk.shaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(gVk.device, gVk.shaderModule, nullptr);
        gVk.shaderModule = VK_NULL_HANDLE;
    }
    if (gVk.device != VK_NULL_HANDLE) {
        vkDestroyDevice(gVk.device, nullptr);
        gVk.device = VK_NULL_HANDLE;
    }
    if (gVk.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(gVk.instance, nullptr);
        gVk.instance = VK_NULL_HANDLE;
    }

    gVk.isInitialized = false;
    LOGI("Zakura Vulkan Processor destroyed cleanly.");
}

// =============================================================================
// JNI BRIDGE EXPORTS (Strict com.zakura Package Namespace)
// =============================================================================

JNIEXPORT jboolean JNICALL
Java_com_zakura_VulkanBridge_initVulkan(JNIEnv* env, jobject thiz, jbyteArray shaderBytes, jint precisionMode) {
    jsize len = env->GetArrayLength(shaderBytes);
    jbyte* buf = env->GetByteArrayElements(shaderBytes, nullptr);
    int32_t res = init_vulkan(reinterpret_cast<const uint8_t*>(buf), len, precisionMode);
    env->ReleaseByteArrayElements(shaderBytes, buf, JNI_ABORT);
    return res == 1 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL
Java_com_zakura_VulkanBridge_processImage(JNIEnv* env, jobject thiz,
                                         jbyteArray currPixels, jbyteArray prevPixels,
                                         jint inWidth, jint inHeight,
                                         jint outWidth, jint outHeight,
                                         jfloatArray uniformData,
                                         jfloat shutterAngle, jint smearSamples, jfloat blurIntensity,
                                         jfloat timeSeconds, jint edgeMode, jfloat edgeStrength, jint seed) {
    size_t pixelCount = static_cast<size_t>(outWidth * outHeight);
    jbyte* currPtr = env->GetByteArrayElements(currPixels, nullptr);
    jbyte* prevPtr = prevPixels ? env->GetByteArrayElements(prevPixels, nullptr) : nullptr;
    jfloat* uboPtr = env->GetFloatArrayElements(uniformData, nullptr);
    jsize uboLen = env->GetArrayLength(uniformData);

    jbyteArray resultByteArray = env->NewByteArray(pixelCount * sizeof(uint32_t));
    std::vector<uint8_t> outBuffer(pixelCount * sizeof(uint32_t));

    process_image(reinterpret_cast<const uint8_t*>(currPtr),
                  reinterpret_cast<const uint8_t*>(prevPtr),
                  inWidth, inHeight,
                  outBuffer.data(), outWidth, outHeight,
                  uboPtr, uboLen,
                  shutterAngle, smearSamples, blurIntensity,
                  timeSeconds, edgeMode, edgeStrength, seed);

    env->SetByteArrayRegion(resultByteArray, 0, pixelCount * sizeof(uint32_t),
                            reinterpret_cast<const jbyte*>(outBuffer.data()));

    env->ReleaseByteArrayElements(currPixels, currPtr, JNI_ABORT);
    if (prevPixels && prevPtr) {
        env->ReleaseByteArrayElements(prevPixels, prevPtr, JNI_ABORT);
    }
    env->ReleaseFloatArrayElements(uniformData, uboPtr, JNI_ABORT);

    return resultByteArray;
}

JNIEXPORT jint JNICALL
Java_com_zakura_VulkanBridge_getLastRenderStatus(JNIEnv* env, jobject thiz) {
    return get_last_render_status();
}

JNIEXPORT void JNICALL
Java_com_zakura_VulkanBridge_destroyVulkan(JNIEnv* env, jobject thiz) {
    cleanup_processor();
}

} // extern "C"
