//===-- RewriteCore.cpp - minimal AMDGPU rewrite core ----------*- C++ -*-===//

#include "amdgpu_rewrite_core/RewriteCore.h"

#include "amdgpu_pc_reloc/PatchPlanner.h"
#include "amdgpu_rewrite_core/CodeObjectModel.h"
#include "amdgpu_rewrite_core/SiteAnalysis.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

namespace amdgpu_rewrite_core {
namespace {

using amdgpu_instr_backend::Result;

std::string makeDefaultRewriteId(const RewriteRequest &request) {
  std::ostringstream os;
  os << request.kernelName << "@0x" << std::hex << request.kernelObject;
  return os.str();
}

std::string instrumentationName(InstrumentationLevel level) {
  switch (level) {
  case InstrumentationLevel::noopPatch:
    return "NoopPatch";
  case InstrumentationLevel::dryPayload:
    return "DryPayload";
  case InstrumentationLevel::countingPayload:
    return "CountingPayload";
  }
  return "Unknown";
}

uint64_t textSize(const RewriteRequest &request) {
  if (request.textSize != 0) {
    return request.textSize;
  }
  if (request.codeObjectBytes.size() < request.textOffset) {
    return 0;
  }
  return static_cast<uint64_t>(request.codeObjectBytes.size()) -
         request.textOffset;
}

bool addressToOffset(const RewriteRequest &request, uint64_t address,
                     uint64_t size, size_t &offset) {
  uint64_t sizeInText = textSize(request);
  if (address < request.textBase) {
    return false;
  }
  uint64_t textRelative = address - request.textBase;
  if (textRelative + size > sizeInText) {
    return false;
  }
  offset = static_cast<size_t>(request.textOffset + textRelative);
  return true;
}

const DaisyChainRoute *findRoute(const RewriteRequest &request,
                                 uint32_t siteId) {
  for (const auto &route : request.daisyChains) {
    if (route.siteId == siteId) {
      return &route;
    }
  }
  return nullptr;
}

void addStage(RewriteTrace &trace, std::string name, std::string summary) {
  trace.stages.push_back({std::move(name), std::move(summary), {}});
}

void addInvariant(RewriteTrace &trace, std::string name, InvariantStatus status,
                  uint64_t address, std::string message) {
  trace.invariants.push_back(
      {std::move(name), status, address, std::move(message)});
}

bool overlapsExistingPatch(const RewriteTrace &trace, uint64_t patchPc,
                           uint64_t size) {
  for (const auto &patch : trace.patches) {
    uint64_t patchEnd = patch.pc + patch.newBytes.size();
    uint64_t newEnd = patchPc + size;
    if (patch.pc < newEnd && patchPc < patchEnd) {
      return true;
    }
  }
  return false;
}

Result<bool> applyBranchPatch(const amdgpu_instr_backend::InstructionBackend &backend,
                              const RewriteRequest &request, RewriteResult &result,
                              uint32_t siteId, uint64_t patchPc,
                              uint64_t targetPc, uint64_t overwriteSize,
                              std::string reason) {
  size_t offset = 0;
  if (!addressToOffset(request, patchPc, overwriteSize, offset)) {
    result.diagnostics.push_back(Diagnostic::error(
        DiagnosticCode::patchOutsideText, patchPc,
        "patch address is outside the request text range"));
    addInvariant(result.trace, "patch-inside-text", InvariantStatus::failed,
                 patchPc, "patch address is outside text");
    return Result<bool>::success(false);
  }

  if (overlapsExistingPatch(result.trace, patchPc, overwriteSize)) {
    result.diagnostics.push_back(Diagnostic::error(
        DiagnosticCode::invariantFailed, patchPc,
        "patch range overlaps an already patched range"));
    addInvariant(result.trace, "patched-range-overlap",
                 InvariantStatus::failed, patchPc,
                 "patch range overlaps an existing patch");
    return Result<bool>::success(false);
  }
  addInvariant(result.trace, "patched-range-overlap", InvariantStatus::passed,
               patchPc, "patch range does not overlap previous patches");

  amdgpu_pc_reloc::PatchPlanner planner(backend);
  auto patchOrErr = planner.planSBranchOverwrite({patchPc, targetPc, overwriteSize});
  if (!patchOrErr) {
    return Result<bool>::failure(patchOrErr.error());
  }
  auto patch = patchOrErr.takeValue();
  if (!patch.Diagnostics.empty()) {
    for (const auto &diag : patch.Diagnostics) {
      result.diagnostics.push_back(Diagnostic::error(
          DiagnosticCode::patchOutOfRange, diag.Address, diag.Message));
    }
    addInvariant(result.trace, "branch-reach", InvariantStatus::failed,
                 patchPc, "branch target is outside encodable range");
    return Result<bool>::success(false);
  }

  std::vector<uint8_t> oldBytes(
      result.patched.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
      result.patched.bytes.begin() +
          static_cast<std::ptrdiff_t>(offset + patch.Bytes.size()));
  std::memcpy(result.patched.bytes.data() + offset, patch.Bytes.data(),
              patch.Bytes.size());
  result.trace.patches.push_back(
      {siteId, patchPc, std::move(oldBytes), patch.Bytes, std::move(reason)});
  addInvariant(result.trace, "branch-reach", InvariantStatus::passed, patchPc,
               "branch patch was encodable");
  return Result<bool>::success(true);
}

} // namespace

Result<RewriteResult> Rewriter::rewrite(const RewriteRequest &request,
                                        const RewriteOptions &options) const {
  RewriteResult result;
  result.patched.bytes = request.codeObjectBytes;
  result.patched.textBase = request.textBase;
  result.patched.textOffset = request.textOffset;

  auto parsedOrErr = parseCodeObject(parseRequestFromRewriteRequest(request));
  if (!parsedOrErr) {
    return Result<RewriteResult>::failure(parsedOrErr.error());
  }
  ParsedCodeObject parsed = parsedOrErr.takeValue();
  RewriteRequest effectiveRequest = request;
  effectiveRequest.entryPc = parsed.kernel.entryPc;
  effectiveRequest.textBase = parsed.kernel.textRange.start;
  effectiveRequest.textOffset = parsed.kernel.textOffset;
  effectiveRequest.textSize =
      parsed.kernel.textRange.end - parsed.kernel.textRange.start;
  result.patched.textBase = effectiveRequest.textBase;
  result.patched.textOffset = effectiveRequest.textOffset;

  result.trace.rewriteId =
      request.rewriteId.empty() ? makeDefaultRewriteId(request) : request.rewriteId;
  result.trace.kernel = parsed.kernel;
  result.trace.plan.layout = options.layout;
  result.trace.plan.registerMode = options.registerMode;
  result.trace.plan.zeroSgprFlavor = options.zeroSgprFlavor;
  result.trace.plan.instrumentation = options.instrumentation;
  result.trace.plan.daisyChaining = options.enableDaisyChaining;

  addStage(result.trace, "parse",
           "parsed code object bytes into kernel/text model");
  addInvariant(result.trace, "text-bounds", InvariantStatus::passed,
               parsed.kernel.textRange.start,
               "kernel text range is within code object bytes");

  if (options.layout != RewriteLayout::singleKernelClone ||
      options.registerMode != RegisterMode::zeroSgpr) {
    // TODO: Should this be the way it is? Unsupported live instrumentation
    // modes should be hard failures, not successful results with diagnostics.
    result.diagnostics.push_back(Diagnostic::error(
        DiagnosticCode::unsupportedMode, request.entryPc,
        "new rewrite core supports only SingleKernelClone + ZeroSGPR"));
    return Result<RewriteResult>::success(std::move(result));
  }

  addStage(result.trace, "plan",
           "selected SingleKernelClone + ZeroSGPR with " +
               instrumentationName(options.instrumentation));
  result.trace.payloads.push_back(
      {options.instrumentation, instrumentationName(options.instrumentation), 0});

  std::vector<RewriteSite> rewriteSites = request.sites;
  if (rewriteSites.empty()) {
    auto analysis = analyzeSites(parsed, backend);
    if (!analysis) {
      return Result<RewriteResult>::failure(analysis.error());
    }
    auto siteAnalysis = analysis.takeValue();
    result.trace.analyzedSites = siteAnalysis.siteModels;
    for (const auto &diagnostic : siteAnalysis.diagnostics) {
      result.diagnostics.push_back(diagnostic);
    }
    rewriteSites = std::move(siteAnalysis.rewriteSites);
    addStage(result.trace, "site-analysis",
             "selected " + std::to_string(rewriteSites.size()) +
                 " patchable sites from decoded text");
  } else {
    for (const auto &site : rewriteSites) {
      result.trace.analyzedSites.push_back({site.id, site.patchPc,
                                            site.targetPc, site.overwriteSize,
                                            site.kind,
                                            "selected: provided by caller"});
    }
    addStage(result.trace, "site-analysis",
             "accepted caller-provided rewrite sites");
  }

  if (rewriteSites.empty()) {
    // TODO: Should this be the way it is? No selected sites is useful as an
    // artifact-mode warning, but live instrumentation should treat it as hard
    // failure unless explicitly configured otherwise.
    result.diagnostics.push_back(Diagnostic::warning(
        DiagnosticCode::invalidRequest, parsed.kernel.entryPc,
        "site analysis selected no patchable sites"));
    addInvariant(result.trace, "selected-site-coverage",
                 InvariantStatus::failed, parsed.kernel.entryPc,
                 "no patchable sites selected");
  } else {
    addInvariant(result.trace, "selected-site-coverage",
                 InvariantStatus::passed, parsed.kernel.entryPc,
                 "at least one patchable site selected");
  }

  for (const auto &site : rewriteSites) {
    SiteModel siteModel;
    siteModel.id = site.id;
    siteModel.pc = site.patchPc;
    siteModel.targetPc = site.targetPc;
    siteModel.overwriteSize = site.overwriteSize;
    siteModel.kind = site.kind;
    siteModel.decision = "selected";
    result.trace.plan.selectedSites.push_back(siteModel);

    const DaisyChainRoute *route = findRoute(effectiveRequest, site.id);
    if (route && options.enableDaisyChaining && !route->cellPcs.empty()) {
      std::vector<uint64_t> chain = route->cellPcs;
      result.trace.daisyChains.push_back({site.id, chain, site.targetPc});
      auto first = applyBranchPatch(backend, effectiveRequest, result, site.id,
                                    site.patchPc, chain.front(),
                                    site.overwriteSize,
                                    "site-to-daisy-chain");
      if (!first) {
        return Result<RewriteResult>::failure(first.error());
      }
      for (size_t i = 0; i < chain.size(); ++i) {
        uint64_t target = (i + 1 < chain.size()) ? chain[i + 1] : site.targetPc;
        auto cell = applyBranchPatch(backend, effectiveRequest, result, site.id,
                                     chain[i], target, 4,
                                     "daisy-chain-cell");
        if (!cell) {
          return Result<RewriteResult>::failure(cell.error());
        }
      }
      continue;
    }

    auto direct = applyBranchPatch(backend, effectiveRequest, result, site.id,
                                   site.patchPc, site.targetPc,
                                   site.overwriteSize, "direct-site-patch");
    if (!direct) {
      return Result<RewriteResult>::failure(direct.error());
    }
  }

  addStage(result.trace, "emit",
           "emitted branch patches through shared rewrite pipeline");
  addStage(result.trace, "validate",
           "recorded patch range and branch reach invariants");

  return Result<RewriteResult>::success(std::move(result));
}

} // namespace amdgpu_rewrite_core
