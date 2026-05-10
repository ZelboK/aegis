//===-- RewriteCore.cpp - minimal AMDGPU rewrite core ----------*- C++ -*-===//

#include "amdgpu_rewrite_core/RewriteCore.h"

#include "amdgpu_pc_reloc/PatchPlanner.h"

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

  result.trace.rewriteId =
      request.rewriteId.empty() ? makeDefaultRewriteId(request) : request.rewriteId;
  result.trace.kernel = {request.kernelName,
                         request.arch,
                         request.kernelObject,
                         request.entryPc,
                         {request.textBase, request.textBase + textSize(request)},
                         request.textOffset,
                         {}};
  result.trace.plan.layout = options.layout;
  result.trace.plan.registerMode = options.registerMode;
  result.trace.plan.zeroSgprFlavor = options.zeroSgprFlavor;
  result.trace.plan.instrumentation = options.instrumentation;
  result.trace.plan.daisyChaining = options.enableDaisyChaining;

  addStage(result.trace, "parse",
           "accepted caller-provided code object bytes and text range");

  if (options.layout != RewriteLayout::singleKernelClone ||
      options.registerMode != RegisterMode::zeroSgpr) {
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

  for (const auto &site : request.sites) {
    SiteModel siteModel;
    siteModel.id = site.id;
    siteModel.pc = site.patchPc;
    siteModel.targetPc = site.targetPc;
    siteModel.overwriteSize = site.overwriteSize;
    siteModel.kind = site.kind;
    siteModel.decision = "selected";
    result.trace.plan.selectedSites.push_back(siteModel);

    const DaisyChainRoute *route = findRoute(request, site.id);
    if (route && options.enableDaisyChaining && !route->cellPcs.empty()) {
      std::vector<uint64_t> chain = route->cellPcs;
      result.trace.daisyChains.push_back({site.id, chain, site.targetPc});
      auto first = applyBranchPatch(backend, request, result, site.id,
                                    site.patchPc, chain.front(),
                                    site.overwriteSize,
                                    "site-to-daisy-chain");
      if (!first) {
        return Result<RewriteResult>::failure(first.error());
      }
      for (size_t i = 0; i < chain.size(); ++i) {
        uint64_t target = (i + 1 < chain.size()) ? chain[i + 1] : site.targetPc;
        auto cell = applyBranchPatch(backend, request, result, site.id,
                                     chain[i], target, 4,
                                     "daisy-chain-cell");
        if (!cell) {
          return Result<RewriteResult>::failure(cell.error());
        }
      }
      continue;
    }

    auto direct = applyBranchPatch(backend, request, result, site.id,
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
