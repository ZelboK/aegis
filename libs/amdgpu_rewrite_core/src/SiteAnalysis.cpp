//===-- SiteAnalysis.cpp - first rewrite site analysis ---------*- C++ -*-===//

#include "amdgpu_rewrite_core/SiteAnalysis.h"

#include "amdgpu_instr_backend/ByteView.h"

#include <cstddef>
#include <utility>

namespace amdgpu_rewrite_core {
namespace {

SiteKind siteKindFromInstruction(
    amdgpu_instr_backend::Instruction::MemoryKind memory) {
  switch (memory) {
  case amdgpu_instr_backend::Instruction::MemoryKind::global:
    return SiteKind::globalMemory;
  case amdgpu_instr_backend::Instruction::MemoryKind::lds:
    return SiteKind::ldsMemory;
  case amdgpu_instr_backend::Instruction::MemoryKind::none:
  case amdgpu_instr_backend::Instruction::MemoryKind::other:
    return SiteKind::unknown;
  }
  return SiteKind::unknown;
}

bool isSupportedMemorySite(const amdgpu_instr_backend::Instruction &inst) {
  return (inst.Memory == amdgpu_instr_backend::Instruction::MemoryKind::global ||
          inst.Memory == amdgpu_instr_backend::Instruction::MemoryKind::lds) &&
         (inst.MayLoad || inst.MayStore) && !inst.IsTerminator &&
         !inst.IsPCRelativeBranch && inst.Size != 0;
}

} // namespace

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
    model.kind = siteKindFromInstruction(inst.Memory);

    if (!isSupportedMemorySite(inst)) {
      if (inst.IsPCRelativeBranch || inst.IsTerminator) {
        model.decision = "rejected: control-flow is not a profiling site";
      } else if (inst.Memory == amdgpu_instr_backend::Instruction::MemoryKind::none) {
        model.decision = "rejected: not a memory instruction";
      } else if (inst.Memory == amdgpu_instr_backend::Instruction::MemoryKind::other) {
        model.decision = "rejected: unsupported memory instruction kind";
      } else {
        model.decision = "rejected: unsupported memory-site shape";
      }
      result.siteModels.push_back(std::move(model));
      continue;
    }

    model.targetPc = inst.Address + inst.Size;
    model.decision = "selected: supported memory instruction";
    result.rewriteSites.push_back(
        {nextSiteId, inst.Address, inst.Address + inst.Size, inst.Size,
         model.kind});
    result.siteModels.push_back(std::move(model));
    ++nextSiteId;
  }

  return amdgpu_instr_backend::Result<SiteAnalysisResult>::success(
      std::move(result));
}

} // namespace amdgpu_rewrite_core
