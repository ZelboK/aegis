//===-- HsaKernelLoader.cpp - ROCm-backed kernel loader ---------*- C++ -*-===//

#include "hsa_kernel_loader/HsaKernelLoader.h"

#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hsa_kernel_loader {
namespace {

using amdgpu_instr_backend::Result;

std::string hsaStatusString(hsa_status_t status, const char *context) {
  const char *message = nullptr;
  hsa_status_string(status, &message);
  std::string out = context;
  out += ": ";
  out += message ? message : "unknown HSA error";
  return out;
}

hsa_status_t findGpuAgentCallback(hsa_agent_t agent, void *data) {
  auto *agents = static_cast<std::vector<uint64_t> *>(data);

  hsa_device_type_t deviceType;
  hsa_status_t status =
      hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &deviceType);
  if (status != HSA_STATUS_SUCCESS) {
    return status;
  }

  if (deviceType == HSA_DEVICE_TYPE_GPU) {
    agents->push_back(agent.handle);
  }
  return HSA_STATUS_SUCCESS;
}

struct SymbolSearch {
  std::string targetName;
  hsa_executable_symbol_t symbol{};
  bool found = false;
};

hsa_status_t findSymbolCallback(hsa_executable_t,
                                hsa_executable_symbol_t symbol, void *data) {
  auto *search = static_cast<SymbolSearch *>(data);

  uint32_t nameLength = 0;
  hsa_status_t status = hsa_executable_symbol_get_info(
      symbol, HSA_EXECUTABLE_SYMBOL_INFO_NAME_LENGTH, &nameLength);
  if (status != HSA_STATUS_SUCCESS) {
    return status;
  }

  std::string name(nameLength, '\0');
  status = hsa_executable_symbol_get_info(
      symbol, HSA_EXECUTABLE_SYMBOL_INFO_NAME, &name[0]);
  if (status != HSA_STATUS_SUCCESS) {
    return status;
  }

  if (name == search->targetName) {
    search->symbol = symbol;
    search->found = true;
    return HSA_STATUS_INFO_BREAK;
  }
  return HSA_STATUS_SUCCESS;
}

class HsaKernelLoader final : public KernelLoader {
public:
  explicit HsaKernelLoader(uint64_t agentHandle) : agentHandle_(agentHandle) {}

  ~HsaKernelLoader() override {
    for (uint64_t handle : executableHandles_) {
      hsa_executable_t executable{handle};
      hsa_executable_destroy(executable);
    }
  }

  Result<LoadedKernel> loadKernel(const LoadRequest &request) override {
    if (agentHandle_ == 0) {
      return Result<LoadedKernel>::failure("HSA kernel loader has no GPU agent");
    }
    if (request.codeObjectBytes.empty()) {
      return Result<LoadedKernel>::failure("patched code object bytes are empty");
    }
    if (request.kernelName.empty()) {
      return Result<LoadedKernel>::failure("kernel name is empty");
    }

    hsa_agent_t agent{agentHandle_};
    hsa_code_object_reader_t reader{};
    hsa_status_t status = hsa_code_object_reader_create_from_memory(
        request.codeObjectBytes.data(), request.codeObjectBytes.size(), &reader);
    if (status != HSA_STATUS_SUCCESS) {
      return Result<LoadedKernel>::failure(
          hsaStatusString(status, "failed to create code object reader"));
    }

    hsa_executable_t executable{};
    status = hsa_executable_create_alt(HSA_PROFILE_FULL,
                                       HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,
                                       nullptr, &executable);
    if (status != HSA_STATUS_SUCCESS) {
      hsa_code_object_reader_destroy(reader);
      return Result<LoadedKernel>::failure(
          hsaStatusString(status, "failed to create executable"));
    }

    status = hsa_executable_load_agent_code_object(executable, agent, reader,
                                                  nullptr, nullptr);
    if (status != HSA_STATUS_SUCCESS) {
      hsa_executable_destroy(executable);
      hsa_code_object_reader_destroy(reader);
      return Result<LoadedKernel>::failure(
          hsaStatusString(status, "failed to load code object"));
    }

    status = hsa_executable_freeze(executable, nullptr);
    if (status != HSA_STATUS_SUCCESS) {
      hsa_executable_destroy(executable);
      hsa_code_object_reader_destroy(reader);
      return Result<LoadedKernel>::failure(
          hsaStatusString(status, "failed to freeze executable"));
    }
    hsa_code_object_reader_destroy(reader);

    SymbolSearch search;
    search.targetName = request.kernelName + ".kd";
    status = hsa_executable_iterate_symbols(executable, findSymbolCallback,
                                            &search);
    if (status != HSA_STATUS_SUCCESS && status != HSA_STATUS_INFO_BREAK) {
      hsa_executable_destroy(executable);
      return Result<LoadedKernel>::failure(
          hsaStatusString(status, "failed to iterate executable symbols"));
    }
    if (!search.found) {
      hsa_executable_destroy(executable);
      return Result<LoadedKernel>::failure("kernel descriptor symbol not found: " +
                                           search.targetName);
    }

    uint64_t kernelObject = 0;
    status = hsa_executable_symbol_get_info(
        search.symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &kernelObject);
    if (status != HSA_STATUS_SUCCESS) {
      hsa_executable_destroy(executable);
      return Result<LoadedKernel>::failure(
          hsaStatusString(status, "failed to get kernel object"));
    }

    executableHandles_.push_back(executable.handle);
    LoadedKernel loaded;
    loaded.executableHandle = executable.handle;
    loaded.kernelObject = kernelObject;
    loaded.kernelName = request.kernelName;
    return Result<LoadedKernel>::success(std::move(loaded));
  }

private:
  uint64_t agentHandle_ = 0;
  std::vector<uint64_t> executableHandles_;
};

struct PoolSearch {
  hsa_amd_memory_pool_t pool{};
  bool found = false;
  bool fineGrained = false;
};

hsa_status_t findGlobalPoolCallback(hsa_amd_memory_pool_t pool, void *data) {
  auto *search = static_cast<PoolSearch *>(data);
  hsa_amd_segment_t segment = HSA_AMD_SEGMENT_GLOBAL;
  hsa_status_t status = hsa_amd_memory_pool_get_info(
      pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &segment);
  if (status != HSA_STATUS_SUCCESS) {
    return status;
  }
  if (segment != HSA_AMD_SEGMENT_GLOBAL) {
    return HSA_STATUS_SUCCESS;
  }
  bool allocAllowed = false;
  status = hsa_amd_memory_pool_get_info(
      pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALLOWED, &allocAllowed);
  if (status != HSA_STATUS_SUCCESS) {
    return status;
  }
  if (!allocAllowed) {
    return HSA_STATUS_SUCCESS;
  }
  uint32_t flags = 0;
  status = hsa_amd_memory_pool_get_info(
      pool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &flags);
  if (status != HSA_STATUS_SUCCESS) {
    return status;
  }
  const bool fineGrained =
      (flags & HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_FINE_GRAINED) != 0 ||
      (flags & HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_EXTENDED_SCOPE_FINE_GRAINED) != 0;
  if (!search->found || fineGrained) {
    search->pool = pool;
    search->found = true;
    search->fineGrained = fineGrained;
  }
  if (fineGrained) {
    return HSA_STATUS_INFO_BREAK;
  }
  return HSA_STATUS_SUCCESS;
}

class HsaProfilingBufferAllocator final : public ProfilingBufferAllocator {
public:
  explicit HsaProfilingBufferAllocator(uint64_t agentHandle)
      : agentHandle_(agentHandle) {}

  ~HsaProfilingBufferAllocator() override {
    for (auto buffer : buffers_) {
      release(buffer);
    }
  }

  Result<ProfilingBuffer> allocate(uint64_t size) override {
    if (agentHandle_ == 0) {
      return Result<ProfilingBuffer>::failure(
          "HSA profiling buffer allocator has no GPU agent");
    }
    if (size == 0) {
      return Result<ProfilingBuffer>::failure(
          "profiling buffer size must be non-zero");
    }
    hsa_agent_t agent{agentHandle_};
    PoolSearch search;
    hsa_status_t status =
        hsa_amd_agent_iterate_memory_pools(agent, findGlobalPoolCallback,
                                           &search);
    if (status != HSA_STATUS_SUCCESS && status != HSA_STATUS_INFO_BREAK) {
      return Result<ProfilingBuffer>::failure(
          hsaStatusString(status, "failed to iterate HSA memory pools"));
    }
    if (!search.found) {
      return Result<ProfilingBuffer>::failure(
          "no runtime-allocatable HSA global memory pool found");
    }
    if (!search.fineGrained) {
      return Result<ProfilingBuffer>::failure(
          "no fine-grained HSA global memory pool found for profiling readback");
    }

    void *ptr = nullptr;
    status = hsa_amd_memory_pool_allocate(search.pool, size, 0, &ptr);
    if (status != HSA_STATUS_SUCCESS) {
      return Result<ProfilingBuffer>::failure(
          hsaStatusString(status, "failed to allocate HSA profiling buffer"));
    }
    std::memset(ptr, 0, static_cast<size_t>(size));
    status = hsa_amd_agents_allow_access(1, &agent, nullptr, ptr);
    if (status != HSA_STATUS_SUCCESS) {
      hsa_amd_memory_pool_free(ptr);
      return Result<ProfilingBuffer>::failure(
          hsaStatusString(status,
                          "failed to grant GPU access to profiling buffer"));
    }
    ProfilingBuffer buffer{reinterpret_cast<uint64_t>(ptr), size};
    buffers_.push_back(buffer);
    return Result<ProfilingBuffer>::success(buffer);
  }

  void release(ProfilingBuffer buffer) override {
    if (buffer.address == 0) {
      return;
    }
    hsa_amd_memory_pool_free(reinterpret_cast<void *>(buffer.address));
    buffers_.erase(std::remove_if(buffers_.begin(), buffers_.end(),
                                  [buffer](ProfilingBuffer stored) {
                                    return stored.address == buffer.address;
                                  }),
                   buffers_.end());
  }

private:
  uint64_t agentHandle_ = 0;
  std::vector<ProfilingBuffer> buffers_;
};

class HsaDispatchCompletionObserver final : public DispatchCompletionObserver {
public:
  Result<bool> waitForCompletion(
      const DispatchCompletionRequest &request) override {
    if (request.signalHandle == 0) {
      return Result<bool>::failure(
          "dispatch completion signal is missing");
    }
    hsa_signal_t signal{request.signalHandle};
    const uint64_t timeout = request.timeoutHint == 0
                                 ? std::numeric_limits<uint64_t>::max()
                                 : request.timeoutHint;
    const hsa_signal_value_t value = hsa_signal_wait_scacquire(
        signal, HSA_SIGNAL_CONDITION_LT, 1, timeout, HSA_WAIT_STATE_BLOCKED);
    if (value < 1) {
      return Result<bool>::success(true);
    }
    return Result<bool>::failure(
        "timed out waiting for dispatch completion signal");
  }

  Result<uint64_t> createSignal(uint64_t initialValue) override {
    hsa_signal_t signal{};
    hsa_status_t status =
        hsa_signal_create(static_cast<hsa_signal_value_t>(initialValue), 0,
                          nullptr, &signal);
    if (status != HSA_STATUS_SUCCESS) {
      return Result<uint64_t>::failure(
          hsaStatusString(status, "failed to create completion signal"));
    }
    return Result<uint64_t>::success(signal.handle);
  }

  void destroySignal(uint64_t signalHandle) override {
    if (signalHandle == 0) {
      return;
    }
    hsa_signal_destroy(hsa_signal_t{signalHandle});
  }
};

class HsaAgentResourceProvider final : public AgentResourceProvider {
public:
  KernelLoader *kernelLoaderForAgent(uint64_t agentHandle) override {
    if (agentHandle == 0) {
      return nullptr;
    }
    auto &slot = loaders_[agentHandle];
    if (!slot) {
      slot = std::make_unique<HsaKernelLoader>(agentHandle);
    }
    return slot.get();
  }

  ProfilingBufferAllocator *profilingAllocatorForAgent(
      uint64_t agentHandle) override {
    if (agentHandle == 0) {
      return nullptr;
    }
    auto &slot = allocators_[agentHandle];
    if (!slot) {
      slot = std::make_unique<HsaProfilingBufferAllocator>(agentHandle);
    }
    return slot.get();
  }

  DispatchCompletionObserver *completionObserver() override {
    return &completionObserver_;
  }

private:
  std::map<uint64_t, std::unique_ptr<KernelLoader>> loaders_;
  std::map<uint64_t, std::unique_ptr<ProfilingBufferAllocator>> allocators_;
  HsaDispatchCompletionObserver completionObserver_;
};

} // namespace

std::vector<uint64_t> findGpuAgents() {
  std::vector<uint64_t> agents;
  hsa_status_t status = hsa_init();
  if (status != HSA_STATUS_SUCCESS &&
      status != HSA_STATUS_ERROR_NOT_INITIALIZED) {
    return agents;
  }
  hsa_iterate_agents(findGpuAgentCallback, &agents);
  return agents;
}

uint64_t defaultGpuAgent() {
  auto agents = findGpuAgents();
  return agents.empty() ? 0 : agents.front();
}

Result<std::unique_ptr<KernelLoader>> createHsaKernelLoader(
    uint64_t agentHandle) {
  if (agentHandle == 0) {
    return Result<std::unique_ptr<KernelLoader>>::failure(
        "invalid HSA GPU agent handle");
  }
  return Result<std::unique_ptr<KernelLoader>>::success(
      std::make_unique<HsaKernelLoader>(agentHandle));
}

Result<std::unique_ptr<ProfilingBufferAllocator>>
createHsaProfilingBufferAllocator(uint64_t agentHandle) {
  if (agentHandle == 0) {
    return Result<std::unique_ptr<ProfilingBufferAllocator>>::failure(
        "invalid HSA GPU agent handle");
  }
  return Result<std::unique_ptr<ProfilingBufferAllocator>>::success(
      std::make_unique<HsaProfilingBufferAllocator>(agentHandle));
}

std::unique_ptr<DispatchCompletionObserver>
createHsaDispatchCompletionObserver() {
  return std::make_unique<HsaDispatchCompletionObserver>();
}

std::unique_ptr<AgentResourceProvider>
createHsaAgentResourceProvider() {
  return std::make_unique<HsaAgentResourceProvider>();
}

} // namespace hsa_kernel_loader
