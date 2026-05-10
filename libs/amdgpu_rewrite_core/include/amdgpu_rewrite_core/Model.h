//===-- amdgpu_rewrite_core/Model.h -----------------------------*- C++ -*-===//

#ifndef AMDGPU_REWRITE_CORE_MODEL_H
#define AMDGPU_REWRITE_CORE_MODEL_H

#include "amdgpu_rewrite_core/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace amdgpu_rewrite_core {

struct KernelModel {
  std::string name;
  std::string arch;
  uint64_t kernelObject = 0;
  uint64_t entryPc = 0;
  ByteRange textRange;
  uint64_t textOffset = 0;
  std::string originalBytesHash;
};

struct SiteModel {
  uint32_t id = 0;
  uint64_t pc = 0;
  uint64_t targetPc = 0;
  uint64_t overwriteSize = 4;
  SiteKind kind = SiteKind::unknown;
  std::string decision;
};

struct PayloadModel {
  InstrumentationLevel level = InstrumentationLevel::noopPatch;
  std::string description;
  uint64_t byteSize = 0;
};

struct TrampolineModel {
  uint32_t siteId = 0;
  uint64_t entryPc = 0;
  uint64_t returnPc = 0;
  uint64_t byteSize = 0;
  std::string description;
};

struct DaisyChainModel {
  uint32_t siteId = 0;
  std::vector<uint64_t> cells;
  uint64_t finalTargetPc = 0;
};

struct PatchModel {
  uint32_t siteId = 0;
  uint64_t pc = 0;
  std::vector<uint8_t> oldBytes;
  std::vector<uint8_t> newBytes;
  std::string reason;
};

struct DescriptorUpdateModel {
  std::string field;
  uint64_t oldValue = 0;
  uint64_t newValue = 0;
  std::string reason;
};

struct InvariantCheck {
  std::string name;
  InvariantStatus status = InvariantStatus::passed;
  uint64_t address = 0;
  std::string message;
};

struct RewritePlanModel {
  RewriteLayout layout = RewriteLayout::singleKernelClone;
  RegisterMode registerMode = RegisterMode::zeroSgpr;
  ZeroSgprFlavor zeroSgprFlavor = ZeroSgprFlavor::withVgprBump;
  InstrumentationLevel instrumentation = InstrumentationLevel::noopPatch;
  bool daisyChaining = false;
  std::vector<SiteModel> selectedSites;
};

struct StageTrace {
  std::string name;
  std::string summary;
  std::vector<Diagnostic> diagnostics;
};

struct RewriteTrace {
  std::string rewriteId;
  KernelModel kernel;
  RewritePlanModel plan;
  std::vector<StageTrace> stages;
  std::vector<PatchModel> patches;
  std::vector<TrampolineModel> trampolines;
  std::vector<PayloadModel> payloads;
  std::vector<DaisyChainModel> daisyChains;
  std::vector<DescriptorUpdateModel> descriptorUpdates;
  std::vector<InvariantCheck> invariants;
};

} // namespace amdgpu_rewrite_core

#endif // AMDGPU_REWRITE_CORE_MODEL_H
