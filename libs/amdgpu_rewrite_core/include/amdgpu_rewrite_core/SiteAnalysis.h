//===-- amdgpu_rewrite_core/SiteAnalysis.h ----------------------*- C++ -*-===//

#ifndef AMDGPU_REWRITE_CORE_SITE_ANALYSIS_H
#define AMDGPU_REWRITE_CORE_SITE_ANALYSIS_H

#include "amdgpu_instr_backend/Backend.h"
#include "amdgpu_instr_backend/Result.h"
#include "amdgpu_rewrite_core/CodeObjectModel.h"
#include "amdgpu_rewrite_core/RewriteCore.h"

#include <vector>

namespace amdgpu_rewrite_core {

struct SiteAnalysisResult {
  std::vector<RewriteSite> rewriteSites;
  std::vector<SiteModel> siteModels;
  std::vector<Diagnostic> diagnostics;
};

[[nodiscard]] amdgpu_instr_backend::Result<SiteAnalysisResult>
analyzeSites(const ParsedCodeObject &codeObject,
             const amdgpu_instr_backend::InstructionBackend &backend);

} // namespace amdgpu_rewrite_core

#endif // AMDGPU_REWRITE_CORE_SITE_ANALYSIS_H
