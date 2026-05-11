//===-- hsa_kernel_loader/KernelLoader.h ------------------------*- C++ -*-===//

#ifndef HSA_KERNEL_LOADER_KERNEL_LOADER_H
#define HSA_KERNEL_LOADER_KERNEL_LOADER_H

#include "amdgpu_instr_backend/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hsa_kernel_loader {

struct LoadRequest {
  std::vector<uint8_t> codeObjectBytes;
  std::string kernelName;
  uint32_t originalKernargSize = 0;
};

struct LoadedKernel {
  uint64_t executableHandle = 0;
  uint64_t loadedCodeObjectHandle = 0;
  uint64_t kernelObject = 0;
  std::string kernelName;
};

struct ProfilingBuffer {
  uint64_t address = 0;
  uint64_t size = 0;
};

class KernelLoader {
public:
  virtual ~KernelLoader() = default;

  [[nodiscard]] virtual amdgpu_instr_backend::Result<LoadedKernel>
  loadKernel(const LoadRequest &request) = 0;
};

class ProfilingBufferAllocator {
public:
  virtual ~ProfilingBufferAllocator() = default;

  [[nodiscard]] virtual amdgpu_instr_backend::Result<ProfilingBuffer>
  allocate(uint64_t size) = 0;

  virtual void release(ProfilingBuffer buffer) = 0;
};

struct DispatchCompletionRequest {
  uint64_t signalHandle = 0;
  uint64_t timeoutHint = ~uint64_t{0};
};

class DispatchCompletionObserver {
public:
  virtual ~DispatchCompletionObserver() = default;

  [[nodiscard]] virtual amdgpu_instr_backend::Result<bool>
  waitForCompletion(const DispatchCompletionRequest &request) = 0;

  [[nodiscard]] virtual amdgpu_instr_backend::Result<uint64_t>
  createSignal(uint64_t initialValue) {
    (void)initialValue;
    return amdgpu_instr_backend::Result<uint64_t>::failure(
        "dispatch completion observer cannot create HSA signals");
  }

  virtual void destroySignal(uint64_t signalHandle) {
    (void)signalHandle;
  }
};

class AgentResourceProvider {
public:
  virtual ~AgentResourceProvider() = default;

  [[nodiscard]] virtual KernelLoader *kernelLoaderForAgent(
      uint64_t agentHandle) = 0;
  [[nodiscard]] virtual ProfilingBufferAllocator *profilingAllocatorForAgent(
      uint64_t agentHandle) = 0;
  [[nodiscard]] virtual DispatchCompletionObserver *completionObserver() = 0;
};

} // namespace hsa_kernel_loader

#endif // HSA_KERNEL_LOADER_KERNEL_LOADER_H
