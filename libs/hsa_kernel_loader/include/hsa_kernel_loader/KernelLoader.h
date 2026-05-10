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

class KernelLoader {
public:
  virtual ~KernelLoader() = default;

  [[nodiscard]] virtual amdgpu_instr_backend::Result<LoadedKernel>
  loadKernel(const LoadRequest &request) = 0;
};

} // namespace hsa_kernel_loader

#endif // HSA_KERNEL_LOADER_KERNEL_LOADER_H
