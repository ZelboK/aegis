//===-- ExecutableCapture.cpp - HSA code object capture ---------*- C++ -*-===//

#include "Internal.h"

#if defined(AEGIS_HAS_HSA_INTERCEPT)

#include <hsa/hsa.h>
#include <hsa/hsa_ven_amd_loader.h>

#include <dlfcn.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aegis::hsa_intercept::detail {

namespace {

using HsaExecutableFreezeFn =
    hsa_status_t (*)(hsa_executable_t executable, const char* options);

HsaExecutableFreezeFn realHsaExecutableFreeze() {
  static auto realFn = reinterpret_cast<HsaExecutableFreezeFn>(
      dlsym(RTLD_NEXT, "hsa_executable_freeze"));
  return realFn;
}

bool queryLoaderTable(hsa_ven_amd_loader_1_01_pfn_t& table) {
  std::memset(&table, 0, sizeof(table));
  return hsa_system_get_major_extension_table(
             HSA_EXTENSION_AMD_LOADER, 1, sizeof(table), &table) ==
             HSA_STATUS_SUCCESS &&
         table.hsa_ven_amd_loader_executable_iterate_loaded_code_objects &&
         table.hsa_ven_amd_loader_loaded_code_object_get_info;
}

std::string trimKernelDescriptorSuffix(std::string name) {
  if (name.size() >= 3 && name.compare(name.size() - 3, 3, ".kd") == 0) {
    name.resize(name.size() - 3);
  }
  return name;
}

std::string percentDecode(std::string_view encoded) {
  std::string result;
  result.reserve(encoded.size());
  for (size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.size() &&
        std::isxdigit(static_cast<unsigned char>(encoded[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(encoded[i + 2]))) {
      auto hexValue = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') {
          return static_cast<uint8_t>(c - '0');
        }
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return static_cast<uint8_t>(10 + (c - 'a'));
      };
      result.push_back(static_cast<char>((hexValue(encoded[i + 1]) << 4) |
                                         hexValue(encoded[i + 2])));
      i += 2;
      continue;
    }
    result.push_back(encoded[i]);
  }
  return result;
}

bool parseUriInteger(std::string_view query, std::string_view key,
                     uint64_t& value) {
  size_t keyPos = query.find(key);
  if (keyPos == std::string_view::npos) {
    return false;
  }
  keyPos += key.size();
  size_t end = query.find('&', keyPos);
  std::string number(query.substr(
      keyPos, end == std::string_view::npos ? std::string_view::npos
                                            : end - keyPos));
  int base = 10;
  if (number.size() > 2 && number[0] == '0' &&
      (number[1] == 'x' || number[1] == 'X')) {
    number.erase(0, 2);
    base = 16;
  }
  try {
    value = std::stoull(number, nullptr, base);
    return true;
  } catch (...) {
    return false;
  }
}

bool loadBytesFromUri(std::string_view uri, std::vector<uint8_t>& bytes) {
  constexpr std::string_view fileScheme = "file://";
  if (uri.substr(0, fileScheme.size()) != fileScheme) {
    return false;
  }

  std::string_view payload = uri.substr(fileScheme.size());
  size_t marker = payload.find_first_of("#?");
  std::string_view pathPart =
      marker == std::string_view::npos ? payload : payload.substr(0, marker);
  std::string_view query =
      marker == std::string_view::npos ? std::string_view()
                                       : payload.substr(marker + 1);

  uint64_t offset = 0;
  uint64_t size = 0;
  const bool hasOffset = parseUriInteger(query, "offset=", offset);
  const bool hasSize = parseUriInteger(query, "size=", size);
  if (!hasSize) {
    return false;
  }

  std::ifstream input(percentDecode(pathPart), std::ios::binary);
  if (!input) {
    return false;
  }

  input.seekg(static_cast<std::streamoff>(hasOffset ? offset : 0));
  bytes.resize(static_cast<size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(size));
  return input.good() || input.gcount() == static_cast<std::streamsize>(size);
}

std::vector<hsa_agent_t> gpuAgents() {
  std::vector<hsa_agent_t> agents;
  hsa_iterate_agents(
      [](hsa_agent_t agent, void* data) -> hsa_status_t {
        hsa_device_type_t deviceType = HSA_DEVICE_TYPE_CPU;
        if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &deviceType) !=
            HSA_STATUS_SUCCESS) {
          return HSA_STATUS_SUCCESS;
        }
        if (deviceType == HSA_DEVICE_TYPE_GPU) {
          static_cast<std::vector<hsa_agent_t>*>(data)->push_back(agent);
        }
        return HSA_STATUS_SUCCESS;
      },
      &agents);
  return agents;
}

const CapturedCodeObject* lookupCodeObjectForKernelObjectLocked(
    uint64_t kernelObject) {
  for (auto& [id, codeObject] : state().codeObjects) {
    (void)id;
    if (codeObject.loadBase == 0 || codeObject.loadSize == 0) {
      continue;
    }
    if (kernelObject >= codeObject.loadBase &&
        kernelObject < codeObject.loadBase + codeObject.loadSize) {
      return &codeObject;
    }
  }
  return nullptr;
}

void handleCodeObjectLoad(hsa_loaded_code_object_t loadedCodeObject) {
  hsa_ven_amd_loader_1_01_pfn_t loaderTable{};
  if (!queryLoaderTable(loaderTable)) {
    return;
  }

  CapturedCodeObject codeObject;
  codeObject.codeObjectId = loadedCodeObject.handle;

  uint32_t uriLength = 0;
  loaderTable.hsa_ven_amd_loader_loaded_code_object_get_info(
      loadedCodeObject, HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_URI_LENGTH,
      &uriLength);
  if (uriLength > 0) {
    std::vector<char> uri(static_cast<size_t>(uriLength) + 1, '\0');
    if (loaderTable.hsa_ven_amd_loader_loaded_code_object_get_info(
            loadedCodeObject, HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_URI,
            uri.data()) == HSA_STATUS_SUCCESS) {
      codeObject.uri = uri.data();
    }
  }

  loaderTable.hsa_ven_amd_loader_loaded_code_object_get_info(
      loadedCodeObject, HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_BASE,
      &codeObject.loadBase);
  loaderTable.hsa_ven_amd_loader_loaded_code_object_get_info(
      loadedCodeObject, HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_SIZE,
      &codeObject.loadSize);

  uint32_t storageType = 0;
  loaderTable.hsa_ven_amd_loader_loaded_code_object_get_info(
      loadedCodeObject,
      HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_CODE_OBJECT_STORAGE_TYPE,
      &storageType);

  if (storageType == HSA_VEN_AMD_LOADER_CODE_OBJECT_STORAGE_TYPE_MEMORY) {
    uint64_t memoryBase = 0;
    uint64_t memorySize = 0;
    if (loaderTable.hsa_ven_amd_loader_loaded_code_object_get_info(
            loadedCodeObject,
            HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_CODE_OBJECT_STORAGE_MEMORY_BASE,
            &memoryBase) == HSA_STATUS_SUCCESS &&
        loaderTable.hsa_ven_amd_loader_loaded_code_object_get_info(
            loadedCodeObject,
            HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_CODE_OBJECT_STORAGE_MEMORY_SIZE,
            &memorySize) == HSA_STATUS_SUCCESS &&
        memoryBase != 0 && memorySize != 0) {
      const auto* base = reinterpret_cast<const uint8_t*>(memoryBase);
      codeObject.bytes.assign(base, base + memorySize);
    }
  } else if (!codeObject.uri.empty()) {
    loadBytesFromUri(codeObject.uri, codeObject.bytes);
  }

  Callbacks callbacks;
  CapturedCodeObject stored;
  bool isNew = false;
  {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto [it, inserted] =
        s.codeObjects.insert_or_assign(codeObject.codeObjectId, codeObject);
    isNew = inserted;
    stored = it->second;
    callbacks = s.callbacks;
  }

  if (isNew && callbacks.onCodeObject) {
    callbacks.onCodeObject(stored);
  }
}

void handleKernelSymbolRegister(hsa_agent_t agent,
                                hsa_executable_symbol_t symbol) {
  (void)agent;

  hsa_symbol_kind_t symbolKind = HSA_SYMBOL_KIND_VARIABLE;
  if (hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_TYPE,
                                     &symbolKind) != HSA_STATUS_SUCCESS ||
      symbolKind != HSA_SYMBOL_KIND_KERNEL) {
    return;
  }

  uint32_t nameLength = 0;
  if (hsa_executable_symbol_get_info(
          symbol, HSA_EXECUTABLE_SYMBOL_INFO_NAME_LENGTH,
          &nameLength) != HSA_STATUS_SUCCESS ||
      nameLength == 0) {
    return;
  }

  std::string kernelName(nameLength, '\0');
  if (hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_NAME,
                                     kernelName.data()) != HSA_STATUS_SUCCESS) {
    return;
  }
  if (!kernelName.empty() && kernelName.back() == '\0') {
    kernelName.pop_back();
  }

  CapturedKernelSymbol kernelSymbol;
  kernelSymbol.kernelId = symbol.handle;
  kernelSymbol.kernelName = trimKernelDescriptorSuffix(kernelName);

  hsa_executable_symbol_get_info(symbol,
                                 HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,
                                 &kernelSymbol.kernelObject);
  hsa_executable_symbol_get_info(
      symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,
      &kernelSymbol.kernargSegmentSize);
  hsa_executable_symbol_get_info(
      symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,
      &kernelSymbol.groupSegmentSize);
  hsa_executable_symbol_get_info(
      symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,
      &kernelSymbol.privateSegmentSize);

  Callbacks callbacks;
  {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (const auto* codeObject =
            lookupCodeObjectForKernelObjectLocked(kernelSymbol.kernelObject)) {
      kernelSymbol.codeObjectId = codeObject->codeObjectId;
    }
    callbacks = s.callbacks;
  }

  if (callbacks.onKernelSymbol) {
    callbacks.onKernelSymbol(kernelSymbol);
  }
}

void enumerateExecutableState(hsa_executable_t executable) {
  hsa_ven_amd_loader_1_01_pfn_t loaderTable{};
  if (!queryLoaderTable(loaderTable)) {
    log("AMD loader extension table unavailable during executable freeze");
    return;
  }

  loaderTable.hsa_ven_amd_loader_executable_iterate_loaded_code_objects(
      executable,
      [](hsa_executable_t, hsa_loaded_code_object_t loadedCodeObject,
         void*) -> hsa_status_t {
        handleCodeObjectLoad(loadedCodeObject);
        return HSA_STATUS_SUCCESS;
      },
      nullptr);

  for (hsa_agent_t agent : gpuAgents()) {
    hsa_executable_iterate_agent_symbols(
        executable, agent,
        [](hsa_executable_t, hsa_agent_t agent,
           hsa_executable_symbol_t symbol, void*) -> hsa_status_t {
          handleKernelSymbolRegister(agent, symbol);
          return HSA_STATUS_SUCCESS;
        },
        nullptr);
  }
}

} // namespace

void captureExecutableState(uint64_t executableHandle) {
  auto& s = state();
  if (!s.installed.load()) {
    return;
  }

  bool enabled = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    enabled = s.options.enabled;
  }
  if (!enabled) {
    return;
  }

  hsa_executable_t executable{};
  executable.handle = executableHandle;
  enumerateExecutableState(executable);
}

} // namespace aegis::hsa_intercept::detail

extern "C" hsa_status_t hsa_executable_freeze(hsa_executable_t executable,
                                              const char* options) {
  auto realFn = aegis::hsa_intercept::detail::realHsaExecutableFreeze();
  if (!realFn) {
    return HSA_STATUS_ERROR;
  }

  // #region agent log
  aegis::hsa_intercept::detail::debugLog(
      "ExecutableCapture.cpp:hsa_executable_freeze:entry",
      "hsa_executable_freeze entered", "H5",
      std::string("{\"executable\":") + std::to_string(executable.handle) +
          "}");
  // #endregion
  hsa_status_t status = realFn(executable, options);
  // #region agent log
  aegis::hsa_intercept::detail::debugLog(
      "ExecutableCapture.cpp:hsa_executable_freeze:after-real",
      "real hsa_executable_freeze returned", "H5",
      std::string("{\"status\":") + std::to_string(static_cast<int>(status)) +
          "}");
  // #endregion
  if (status == HSA_STATUS_SUCCESS) {
    aegis::hsa_intercept::detail::captureExecutableState(executable.handle);
  }
  return status;
}

#endif // AEGIS_HAS_HSA_INTERCEPT
