//===-- hsa_kernel_loader/HsaKernelLoader.h ---------------------*- C++ -*-===//

#ifndef HSA_KERNEL_LOADER_HSA_KERNEL_LOADER_H
#define HSA_KERNEL_LOADER_HSA_KERNEL_LOADER_H

#include "hsa_kernel_loader/KernelLoader.h"

#include <memory>
#include <vector>

namespace hsa_kernel_loader {

[[nodiscard]] std::vector<uint64_t> findGpuAgents();
[[nodiscard]] uint64_t defaultGpuAgent();

[[nodiscard]] amdgpu_instr_backend::Result<std::unique_ptr<KernelLoader>>
createHsaKernelLoader(uint64_t agentHandle);

[[nodiscard]] amdgpu_instr_backend::Result<
    std::unique_ptr<ProfilingBufferAllocator>>
createHsaProfilingBufferAllocator(uint64_t agentHandle);

[[nodiscard]] std::unique_ptr<DispatchCompletionObserver>
createHsaDispatchCompletionObserver();

[[nodiscard]] std::unique_ptr<AgentResourceProvider>
createHsaAgentResourceProvider();

} // namespace hsa_kernel_loader

#endif // HSA_KERNEL_LOADER_HSA_KERNEL_LOADER_H
