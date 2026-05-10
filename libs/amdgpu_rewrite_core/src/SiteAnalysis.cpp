//===-- SiteAnalysis.cpp - first rewrite site analysis ---------*- C++ -*-===//

#include "amdgpu_rewrite_core/SiteAnalysis.h"

#include "amdgpu_instr_backend/ByteView.h"

#include <cstddef>
#include <utility>

namespace amdgpu_rewrite_core {

amdgpu_instr_backend::Result<SiteAnalysisResult>
analyzeSites(const ParsedCodeObject &codeObject,
             const amdgpu_instr_backend::InstructionBackend &backend) {
  SiteAnalysisResult result;
  const auto &kernel = codeObject.kernel;
  if (kernel.textRange.empty()) {
    result.diagnostics.push_back(Diagnostic::warning(
        DiagnosticCode::invalidRequest, kernel.entryPc,
        "kernel text range is empty; no sites selected"));
    return amdgpu_instr_backend::Result<SiteAnalysisResult>::success(
        std::move(result));
  }

  uint64_t textSize = kernel.textRange.end - kernel.textRange.start;
  if (kernel.textOffset + textSize > codeObject.bytes.size()) {
    return amdgpu_instr_backend::Result<SiteAnalysisResult>::failure(
        "kernel text range extends beyond code object bytes");
  }

  amdgpu_instr_backend::ByteView text(
      codeObject.bytes.data() + kernel.textOffset, static_cast<size_t>(textSize));
  auto instructions = backend.decodeAll(text, kernel.textRange.start);
  if (!instructions) {
    return amdgpu_instr_backend::Result<SiteAnalysisResult>::failure(
        instructions.error());
  }

  uint32_t nextSiteId = 1;
  for (const auto &inst : instructions.value()) {
    SiteModel model;
    model.id = nextSiteId;
    model.pc = inst.Address;
    model.overwriteSize = inst.Size;
    model.kind = SiteKind::unknown;

    if (!inst.IsPCRelativeBranch) {
      model.decision = "rejected: not a PC-relative branch candidate";
      result.siteModels.push_back(std::move(model));
      continue;
    }

    auto target = backend.branchTarget(inst, inst.Address);
    if (!target) {
      model.decision = "rejected: branch target unavailable";
      result.siteModels.push_back(std::move(model));
      continue;
    }

    model.targetPc = target.value();
    model.decision = "selected: PC-relative branch candidate";
    result.rewriteSites.push_back(
        {nextSiteId, inst.Address, target.value(), inst.Size, SiteKind::unknown});
    result.siteModels.push_back(std::move(model));
    ++nextSiteId;
  }

  return amdgpu_instr_backend::Result<SiteAnalysisResult>::success(
      std::move(result));
}

} // namespace amdgpu_rewrite_core
