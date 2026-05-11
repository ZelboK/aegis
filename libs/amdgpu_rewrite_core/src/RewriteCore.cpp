//===-- RewriteCore.cpp - minimal AMDGPU rewrite core ----------*- C++ -*-===//

#include "amdgpu_rewrite_core/RewriteCore.h"

#include "amdgpu_pc_reloc/PatchPlanner.h"
#include "amdgpu_rewrite_core/CodeObjectMutation.h"
#include "amdgpu_rewrite_core/CodeObjectModel.h"
#include "amdgpu_rewrite_core/CountingPayloadAbi.h"
#include "amdgpu_rewrite_core/PayloadEmitter.h"
#include "amdgpu_rewrite_core/SiteAnalysis.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
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

bool appendTextBytes(RewriteResult &result, RewriteRequest &request,
                     const std::vector<uint8_t> &payloadBytes,
                     uint64_t &entryPc, std::string &error) {
  TextAppendRequest appendRequest;
  appendRequest.codeObjectBytes = result.patched.bytes;
  appendRequest.textBase = result.trace.kernel.textSectionBase == 0
                               ? request.textBase
                               : result.trace.kernel.textSectionBase;
  appendRequest.textOffset = result.trace.kernel.textSectionOffset == 0
                                 ? request.textOffset
                                 : result.trace.kernel.textSectionOffset;
  appendRequest.textSize = result.trace.kernel.textSectionSize == 0
                               ? textSize(request)
                               : result.trace.kernel.textSectionSize;
  appendRequest.bytesToAppend = payloadBytes;
  auto appendResult = appendToText(appendRequest);
  if (!appendResult) {
    error = appendResult.error();
    return false;
  }
  auto value = appendResult.takeValue();
  result.patched.bytes = std::move(value.codeObjectBytes);
  entryPc = value.appendedPc;
  result.trace.kernel.textSectionSize = value.textSize;
  return true;
}

std::string patchReason(InstrumentationLevel level) {
  switch (level) {
  case InstrumentationLevel::noopPatch:
    return "direct-site-patch";
  case InstrumentationLevel::dryPayload:
    return "dry-payload-detour";
  case InstrumentationLevel::countingPayload:
    return "counting-payload-detour";
  }
  return "direct-site-patch";
}

uint32_t roundUp(uint32_t value, uint32_t granularity) {
  return ((value + granularity - 1) / granularity) * granularity;
}

uint32_t encodeVgprCount(uint32_t pgmRsrc1, uint32_t vgprCount,
                         uint32_t granularity) {
  const uint32_t rounded = roundUp(vgprCount, granularity);
  const uint32_t granulated = (rounded / granularity) - 1;
  return (pgmRsrc1 & ~0x3fu) | (granulated & 0x3fu);
}

uint32_t decodeAccumOffset(uint32_t pgmRsrc3) {
  if (pgmRsrc3 == 0) {
    return 0;
  }
  return ((pgmRsrc3 & 0x3fu) + 1) * 4;
}

struct CountingScratchPlan {
  unsigned tempVgprBaseIndex = 0;
  bool useAgprSpill = false;
  unsigned agprSpillBaseIndex = 0;
};

void writeLe32(std::vector<uint8_t> &bytes, uint64_t offset, uint32_t value) {
  if (offset + 4 > bytes.size()) {
    return;
  }
  for (uint64_t i = 0; i < 4; ++i) {
    bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
  }
}

uint32_t defaultVgprGranularity(const std::string &arch) {
  if (arch.rfind("gfx90a", 0) == 0 || arch.rfind("gfx940", 0) == 0 ||
      arch.rfind("gfx942", 0) == 0 || arch.rfind("gfx950", 0) == 0 ||
      arch.rfind("gfx1250", 0) == 0) {
    return 8;
  }
  return 4;
}

void applyDescriptorUpdates(RewriteResult &result,
                            const RewriteOptions &options) {
  auto &kernel = result.trace.kernel;
  if (options.instrumentation == InstrumentationLevel::noopPatch ||
      options.zeroSgprFlavor != ZeroSgprFlavor::withVgprBump ||
      !kernel.descriptorPresent || kernel.vgprCount == 0) {
    return;
  }

  constexpr uint32_t payloadVgprs = 3;
  constexpr uint32_t accumSpillSlots = 4;
  const uint32_t oldAccumOffset = decodeAccumOffset(kernel.computePgmRsrc3);
  const bool hasAccumOffset =
      oldAccumOffset != 0 && oldAccumOffset <= kernel.vgprCount;
  const uint32_t granularity =
      kernel.vgprGranularity == 0 ? defaultVgprGranularity(kernel.arch)
                                  : kernel.vgprGranularity;
  const uint32_t requiredExtraVgprs =
      hasAccumOffset ? accumSpillSlots : payloadVgprs;
  const uint32_t newVgprCount =
      roundUp(kernel.vgprCount + requiredExtraVgprs, granularity);
  const uint32_t newPgmRsrc1 =
      encodeVgprCount(kernel.computePgmRsrc1, newVgprCount, granularity);
  if (kernel.descriptorOffset + 52 <= result.patched.bytes.size()) {
    if (hasAccumOffset) {
      result.trace.descriptorUpdates.push_back(
          {"agpr_spill_base", kernel.vgprCount - oldAccumOffset,
           kernel.vgprCount - oldAccumOffset + accumSpillSlots,
           "CountingPayload spills borrowed regular VGPRs into new AGPR slots"});
    }
    writeLe32(result.patched.bytes, kernel.descriptorOffset + 48, newPgmRsrc1);
    result.trace.descriptorUpdates.push_back(
        {"vgpr_count", kernel.vgprCount, newVgprCount,
         hasAccumOffset
             ? "ZeroSGPR with VGPR bump reserves AGPR spill slots while "
               "keeping accum_offset stable"
             : "ZeroSGPR with VGPR bump reserves payload VGPRs"});
    result.trace.descriptorUpdates.push_back(
        {"compute_pgm_rsrc1", kernel.computePgmRsrc1, newPgmRsrc1,
         "encoded bumped VGPR count"});
    kernel.vgprCount = newVgprCount;
    kernel.computePgmRsrc1 = newPgmRsrc1;
    addInvariant(result.trace, "descriptor-vgpr-bump",
                 InvariantStatus::passed, kernel.descriptorOffset,
                 "descriptor VGPR count updated for payload scratch");
  } else {
    addInvariant(result.trace, "descriptor-vgpr-bump",
                 InvariantStatus::failed, kernel.descriptorOffset,
                 "descriptor offset is outside code object bytes");
  }
}

Result<CountingScratchPlan> countingPayloadScratchPlan(
    const RewriteTrace &trace, const RewriteOptions &options) {
  if (options.zeroSgprFlavor != ZeroSgprFlavor::withVgprBump) {
    return Result<CountingScratchPlan>::failure(
        "CountingPayload currently requires ZeroSGPR with VGPR bump");
  }
  uint64_t oldVgprCount = 0;
  std::optional<uint64_t> agprSpillBase;
  for (const auto &update : trace.descriptorUpdates) {
    if (update.field == "agpr_spill_base") {
      agprSpillBase = update.oldValue;
    }
    if (update.field == "vgpr_count") {
      oldVgprCount = update.oldValue;
    }
  }
  if (oldVgprCount == 0) {
    return Result<CountingScratchPlan>::failure(
      "CountingPayload requires descriptor VGPR facts for payload scratch "
      "registers");
  }
  const uint32_t oldAccumOffset = decodeAccumOffset(trace.kernel.computePgmRsrc3);
  CountingScratchPlan plan;
  if (oldAccumOffset != 0 && oldAccumOffset <= oldVgprCount) {
    if (oldAccumOffset < 3 || !agprSpillBase.has_value()) {
      return Result<CountingScratchPlan>::failure(
          "CountingPayload requires at least three regular VGPRs below "
          "accum_offset for AGPR-backed scratch");
    }
    plan.tempVgprBaseIndex = static_cast<unsigned>(oldAccumOffset - 3);
    plan.useAgprSpill = true;
    plan.agprSpillBaseIndex = static_cast<unsigned>(*agprSpillBase);
    return Result<CountingScratchPlan>::success(plan);
  }
  plan.tempVgprBaseIndex = static_cast<unsigned>(oldVgprCount);
  return Result<CountingScratchPlan>::success(plan);
}

Result<std::vector<uint8_t>>
encodeBranchToTarget(const amdgpu_instr_backend::InstructionBackend &backend,
                     uint64_t branchPc, uint64_t targetPc) {
  const auto byteDelta = static_cast<int64_t>(targetPc) -
                         static_cast<int64_t>(branchPc + 4);
  if (byteDelta % 4 != 0) {
    return Result<std::vector<uint8_t>>::failure(
        "branch target is not dword aligned");
  }
  const int64_t dwordDelta = byteDelta / 4;
  if (dwordDelta < std::numeric_limits<int16_t>::min() ||
      dwordDelta > std::numeric_limits<int16_t>::max()) {
    return Result<std::vector<uint8_t>>::failure(
        "branch target is outside direct s_branch reach");
  }
  return backend.encodeSBranch(static_cast<int16_t>(dwordDelta));
}

Result<std::vector<uint8_t>>
buildIslandBytes(const amdgpu_instr_backend::InstructionBackend &backend,
                 const RewriteRequest &request, const RewriteResult &result,
                 const RewriteSite &site,
                 const std::vector<uint8_t> &payloadBytes,
                 uint64_t islandEntryPc) {
  size_t displacedOffset = 0;
  if (!addressToOffset(request, site.patchPc, site.overwriteSize,
                       displacedOffset)) {
    return Result<std::vector<uint8_t>>::failure(
        "displaced instruction is outside text");
  }

  std::vector<uint8_t> island;
  island.insert(island.end(), payloadBytes.begin(), payloadBytes.end());
  island.insert(island.end(),
                result.patched.bytes.begin() +
                    static_cast<std::ptrdiff_t>(displacedOffset),
                result.patched.bytes.begin() +
                    static_cast<std::ptrdiff_t>(displacedOffset +
                                                site.overwriteSize));
  const uint64_t branchPc = islandEntryPc + island.size();
  const uint64_t returnPc = site.patchPc + site.overwriteSize;
  auto branchBack = encodeBranchToTarget(backend, branchPc, returnPc);
  if (!branchBack) {
    return Result<std::vector<uint8_t>>::failure(branchBack.error());
  }
  auto branchBytes = branchBack.takeValue();
  island.insert(island.end(), branchBytes.begin(), branchBytes.end());
  return Result<std::vector<uint8_t>>::success(std::move(island));
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
    result.diagnostics.push_back(Diagnostic::error(
        DiagnosticCode::unsupportedMode, request.entryPc,
        "new rewrite core supports only SingleKernelClone + ZeroSGPR"));
    return Result<RewriteResult>::success(std::move(result));
  }

  addStage(result.trace, "plan",
           "selected SingleKernelClone + ZeroSGPR with " +
               instrumentationName(options.instrumentation));
  PayloadModel baselinePayload;
  baselinePayload.level = InstrumentationLevel::noopPatch;
  baselinePayload.description = "NoopPatch baseline branch patch";
  result.trace.payloads.push_back(std::move(baselinePayload));

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
    if (options.instrumentation == InstrumentationLevel::noopPatch) {
      addInvariant(result.trace, "noopatch-preserves-bytes",
                   InvariantStatus::passed, parsed.kernel.entryPc,
                   "NoopPatch does not mutate kernel text");
    } else {
      result.diagnostics.push_back(Diagnostic::warning(
          DiagnosticCode::invalidRequest, parsed.kernel.entryPc,
          "site analysis selected no patchable sites"));
      addInvariant(result.trace, "selected-site-coverage",
                   InvariantStatus::failed, parsed.kernel.entryPc,
                   "no patchable sites selected");
    }
  } else {
    addInvariant(result.trace, "selected-site-coverage",
                 InvariantStatus::passed, parsed.kernel.entryPc,
                 "at least one patchable site selected");
  }

  if (options.instrumentation == InstrumentationLevel::noopPatch) {
    addStage(result.trace, "emit",
             "NoopPatch preserved code object bytes without patching sites");
    addStage(result.trace, "validate",
             "recorded parse and site-analysis facts for live redirection");
    return Result<RewriteResult>::success(std::move(result));
  }

  applyDescriptorUpdates(result, options);
  CountingScratchPlan countingScratchPlan;
  if (options.instrumentation == InstrumentationLevel::countingPayload) {
    auto scratchPlan = countingPayloadScratchPlan(result.trace, options);
    if (!scratchPlan) {
      return Result<RewriteResult>::failure(scratchPlan.error());
    }
    countingScratchPlan = scratchPlan.takeValue();
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

    uint64_t patchTargetPc = site.targetPc;
    if (options.instrumentation != InstrumentationLevel::noopPatch) {
      const uint64_t payloadEntryPc =
          (result.trace.kernel.textSectionBase == 0
               ? effectiveRequest.textBase
               : result.trace.kernel.textSectionBase) +
          (result.trace.kernel.textSectionSize == 0
               ? textSize(effectiveRequest)
               : result.trace.kernel.textSectionSize);
      PayloadEmissionRequest payloadRequest;
      payloadRequest.level = options.instrumentation;
      payloadRequest.siteId = site.id;
      payloadRequest.entryPc = payloadEntryPc;
      payloadRequest.returnPc = site.targetPc;
      payloadRequest.profilingBufferAddress =
          effectiveRequest.profilingBufferAddress;
      payloadRequest.profilingBufferSize = effectiveRequest.profilingBufferSize;
      payloadRequest.tempVgprBaseIndex =
          countingScratchPlan.tempVgprBaseIndex;
      payloadRequest.useAgprSpill = countingScratchPlan.useAgprSpill;
      payloadRequest.agprSpillBaseIndex =
          countingScratchPlan.agprSpillBaseIndex;
      auto payload = emitPayload(backend, payloadRequest);
      if (!payload) {
        return Result<RewriteResult>::failure(payload.error());
      }
      auto emission = payload.takeValue();
      auto island = buildIslandBytes(backend, effectiveRequest, result, site,
                                     emission.bytes, payloadEntryPc);
      if (!island) {
        return Result<RewriteResult>::failure(island.error());
      }
      auto islandBytes = island.takeValue();
      uint64_t appendedEntryPc = 0;
      std::string appendError;
      if (!appendTextBytes(result, effectiveRequest, islandBytes,
                           appendedEntryPc, appendError)) {
        return Result<RewriteResult>::failure(appendError);
      }
      emission.entryPc = appendedEntryPc;
      patchTargetPc = emission.entryPc;
      PayloadModel payloadModel;
      payloadModel.level = options.instrumentation;
      payloadModel.description = emission.description;
      payloadModel.byteSize = static_cast<uint64_t>(emission.bytes.size());
      payloadModel.bytes = emission.bytes;
      if (options.instrumentation == InstrumentationLevel::countingPayload) {
        payloadModel.profilingBufferAddress =
            effectiveRequest.profilingBufferAddress;
        payloadModel.profilingRecordSize = countingPayloadRecordV1Size;
        payloadModel.abi = "CountingPayloadRecordV1:baked-buffer-address";
      }
      result.trace.payloads.push_back(std::move(payloadModel));
      result.trace.trampolines.push_back(
          {site.id, emission.entryPc, emission.returnPc,
           static_cast<uint64_t>(islandBytes.size()), emission.description,
           islandBytes});
    }

    const DaisyChainRoute *route = findRoute(effectiveRequest, site.id);
    if (route && options.enableDaisyChaining && !route->cellPcs.empty()) {
      std::vector<uint64_t> chain = route->cellPcs;
      result.trace.daisyChains.push_back({site.id, chain, patchTargetPc});
      auto first = applyBranchPatch(backend, effectiveRequest, result, site.id,
                                    site.patchPc, chain.front(),
                                    site.overwriteSize,
                                    "site-to-daisy-chain");
      if (!first) {
        return Result<RewriteResult>::failure(first.error());
      }
      for (size_t i = 0; i < chain.size(); ++i) {
        uint64_t target = (i + 1 < chain.size()) ? chain[i + 1] : patchTargetPc;
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
                                   site.patchPc, patchTargetPc,
                                   site.overwriteSize,
                                   patchReason(options.instrumentation));
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
