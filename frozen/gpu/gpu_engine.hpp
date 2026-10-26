#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::gpu {

/**
 * @brief Hardware accelerator backend type.
 */
enum class AcceleratorBackend {
    None,
    WgpuVulkan,
    WgpuMetal,
    WgpuDx12,
    Cuda,
    AppleSiliconUma
};

/**
 * @brief Telemetry profile for local accelerator.
 */
struct GpuProfile {
    AcceleratorBackend backend{AcceleratorBackend::None};
    std::string device_name;
    uint64_t total_vram_bytes{0};
    uint64_t free_vram_bytes{0};
    uint32_t compute_units{0};
    double measured_memory_bandwidth_gbps{0.0};
};

/**
 * @brief Heterogeneous Accelerator & GPU Compute Engine.
 * Manages compute dispatch across local GPU devices (via wgpu / CUDA),
 * pools Apple Silicon Unified Memory, and evaluates network bandwidth
 * to ensure only compute-dense kernels are dispatched remotely.
 */
class GpuEngine {
public:
    GpuEngine();
    ~GpuEngine();

    /**
     * @brief Detects available local GPU hardware and initializes backends.
     */
    bool ProbeHardware();

    /**
     * @brief Retrieves active GPU telemetry profile.
     */
    [[nodiscard]] GpuProfile GetProfile() const;

    /**
     * @brief Determines whether a task has sufficient FLOP/byte density to justify remote dispatch.
     */
    [[nodiscard]] bool ShouldOffload(uint64_t estimated_flops,
                                     uint64_t input_bytes,
                                     double link_bandwidth_mbps) const;

    /**
     * @brief Dispatches a portable compute shader or kernel payload.
     */
    bool ExecuteComputeKernel(std::string_view kernel_bytecode,
                              const std::vector<uint8_t>& input_buffer,
                              std::vector<uint8_t>& out_buffer);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeromesh::gpu
