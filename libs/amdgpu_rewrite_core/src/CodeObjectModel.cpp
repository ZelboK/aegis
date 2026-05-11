//===-- CodeObjectModel.cpp - rewrite code object model ---------*- C++ -*-===//

#include "amdgpu_rewrite_core/CodeObjectModel.h"

#include <sstream>
#include <utility>

namespace amdgpu_rewrite_core {
namespace {

std::string hashBytes(const std::vector<uint8_t> &bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  std::ostringstream os;
  os << std::hex << hash;
  return os.str();
}

} // namespace

amdgpu_instr_backend::Result<ParsedCodeObject>
parseCodeObject(const CodeObjectParseRequest &request) {
  if (request.parser) {
    amdgpu_code_object::ParseRequest parserRequest;
    parserRequest.bytes = request.bytes;
    parserRequest.kernelName = request.kernelName;
    auto parsedKernel = request.parser->parseKernel(parserRequest);
    if (!parsedKernel) {
      return amdgpu_instr_backend::Result<ParsedCodeObject>::failure(
          parsedKernel.error());
    }

    auto kernel = parsedKernel.takeValue();
    ParsedCodeObject parsed;
    parsed.bytes = request.bytes;
    parsed.kernel.name = kernel.name;
    parsed.kernel.arch = kernel.arch;
    parsed.kernel.kernelObject = request.kernelObject;
    parsed.kernel.entryPc = kernel.entryPc;
    parsed.kernel.textRange = {kernel.textBase, kernel.textBase + kernel.textSize};
    parsed.kernel.textOffset = kernel.textOffset;
    parsed.kernel.textSectionBase = kernel.textSection.address;
    parsed.kernel.textSectionOffset = kernel.textSection.fileOffset;
    parsed.kernel.textSectionSize = kernel.textSection.size;
    parsed.kernel.metadataSource = "parser";
    parsed.kernel.originalBytesHash = hashBytes(request.bytes);
    if (kernel.descriptor) {
      parsed.kernel.descriptorPresent = kernel.descriptor->present;
      parsed.kernel.descriptorOffset = kernel.descriptor->fileOffset;
      parsed.kernel.descriptorSize = kernel.descriptor->size;
      parsed.kernel.kernargSize = kernel.descriptor->kernargSize;
      parsed.kernel.groupSegmentFixedSize =
          kernel.descriptor->groupSegmentFixedSize;
      parsed.kernel.privateSegmentFixedSize =
          kernel.descriptor->privateSegmentFixedSize;
      parsed.kernel.computePgmRsrc1 = kernel.descriptor->computePgmRsrc1;
      parsed.kernel.computePgmRsrc2 = kernel.descriptor->computePgmRsrc2;
      parsed.kernel.computePgmRsrc3 = kernel.descriptor->computePgmRsrc3;
      parsed.kernel.vgprCount = kernel.descriptor->vgprCount;
      parsed.kernel.sgprCount = kernel.descriptor->sgprCount;
      parsed.kernel.vgprGranularity = kernel.descriptor->vgprGranularity;
    }
    return amdgpu_instr_backend::Result<ParsedCodeObject>::success(
        std::move(parsed));
  }

  if (request.textOffset > request.bytes.size()) {
    return amdgpu_instr_backend::Result<ParsedCodeObject>::failure(
        "text offset is outside code object bytes");
  }

  uint64_t availableText =
      static_cast<uint64_t>(request.bytes.size() - request.textOffset);
  uint64_t textSize = request.textSize.value_or(availableText);
  if (textSize > availableText) {
    return amdgpu_instr_backend::Result<ParsedCodeObject>::failure(
        "text size extends beyond code object bytes");
  }

  ParsedCodeObject parsed;
  parsed.bytes = request.bytes;
  parsed.kernel.name = request.kernelName;
  parsed.kernel.arch = request.arch;
  parsed.kernel.kernelObject = request.kernelObject;
  parsed.kernel.entryPc = request.entryPc;
  parsed.kernel.textRange = {request.textBase, request.textBase + textSize};
  parsed.kernel.textOffset = request.textOffset;
  parsed.kernel.textSectionBase = request.textBase;
  parsed.kernel.textSectionOffset = request.textOffset;
  parsed.kernel.textSectionSize = textSize;
  parsed.kernel.metadataSource = "request";
  parsed.kernel.originalBytesHash = hashBytes(request.bytes);

  return amdgpu_instr_backend::Result<ParsedCodeObject>::success(
      std::move(parsed));
}

CodeObjectParseRequest parseRequestFromRewriteRequest(
    const RewriteRequest &request) {
  CodeObjectParseRequest parse;
  parse.bytes = request.codeObjectBytes;
  parse.kernelName = request.kernelName;
  parse.arch = request.arch;
  parse.kernelObject = request.kernelObject;
  parse.entryPc = request.entryPc;
  parse.textBase = request.textBase;
  parse.textOffset = request.textOffset;
  if (request.textSize != 0) {
    parse.textSize = request.textSize;
  }
  parse.parser = request.codeObjectParser;
  return parse;
}

} // namespace amdgpu_rewrite_core
