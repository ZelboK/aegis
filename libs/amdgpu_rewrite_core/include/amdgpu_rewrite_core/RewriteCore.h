//===-- amdgpu_rewrite_core/RewriteCore.h -----------------------*- C++ -*-===//

#ifndef AMDGPU_REWRITE_CORE_REWRITE_CORE_H
#define AMDGPU_REWRITE_CORE_REWRITE_CORE_H

#include "amdgpu_instr_backend/Backend.h"
#include "amdgpu_instr_backend/Result.h"
#include "amdgpu_rewrite_core/Model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace amdgpu_rewrite_core {

struct RewriteSite {
  uint32_t id = 0;
  uint64_t patchPc = 0;
  uint64_t targetPc = 0;
  uint64_t overwriteSize = 4;
  SiteKind kind = SiteKind::unknown;
};

struct DaisyChainRoute {
  uint32_t siteId = 0;
  std::vector<uint64_t> cellPcs;
};

struct RewriteRequest {
  std::string rewriteId;
  std::string kernelName;
  std::string arch;
  uint64_t kernelObject = 0;
  uint64_t entryPc = 0;
  uint64_t textBase = 0;
  uint64_t textOffset = 0;
  std::vector<uint8_t> codeObjectBytes;
  std::vector<RewriteSite> sites;
  std::vector<DaisyChainRoute> daisyChains;
};

struct RewriteOptions {
  RewriteLayout layout = RewriteLayout::singleKernelClone;
  RegisterMode registerMode = RegisterMode::zeroSgpr;
  ZeroSgprFlavor zeroSgprFlavor = ZeroSgprFlavor::withVgprBump;
  InstrumentationLevel instrumentation = InstrumentationLevel::noopPatch;
  bool enableDaisyChaining = true;
  bool collectTrace = true;
};

struct PatchedCodeObject {
  std::vector<uint8_t> bytes;
  uint64_t textBase = 0;
  uint64_t textOffset = 0;
};

struct RewriteResult {
  PatchedCodeObject patched;
  RewriteTrace trace;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool hasErrors() const {
    for (const auto &diagnostic : diagnostics) {
      if (diagnostic.severity == DiagnosticSeverity::error) {
        return true;
      }
    }
    return false;
  }
};

class Rewriter {
public:
  explicit Rewriter(const amdgpu_instr_backend::InstructionBackend &backend)
      : backend(backend) {}

  [[nodiscard]] amdgpu_instr_backend::Result<RewriteResult>
  rewrite(const RewriteRequest &request, const RewriteOptions &options) const;

private:
  const amdgpu_instr_backend::InstructionBackend &backend;
};

} // namespace amdgpu_rewrite_core

#endif // AMDGPU_REWRITE_CORE_REWRITE_CORE_H
