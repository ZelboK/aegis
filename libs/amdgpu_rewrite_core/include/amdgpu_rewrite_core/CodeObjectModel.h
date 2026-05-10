//===-- amdgpu_rewrite_core/CodeObjectModel.h -------------------*- C++ -*-===//

#ifndef AMDGPU_REWRITE_CORE_CODE_OBJECT_MODEL_H
#define AMDGPU_REWRITE_CORE_CODE_OBJECT_MODEL_H

#include "amdgpu_code_object/CodeObject.h"
#include "amdgpu_instr_backend/Result.h"
#include "amdgpu_rewrite_core/Model.h"
#include "amdgpu_rewrite_core/RewriteCore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace amdgpu_rewrite_core {

struct CodeObjectParseRequest {
  std::vector<uint8_t> bytes;
  std::string kernelName;
  std::string arch;
  uint64_t kernelObject = 0;
  uint64_t entryPc = 0;
  uint64_t textBase = 0;
  uint64_t textOffset = 0;
  std::optional<uint64_t> textSize;
  const amdgpu_code_object::CodeObjectParser *parser = nullptr;
};

struct ParsedCodeObject {
  std::vector<uint8_t> bytes;
  KernelModel kernel;
};

[[nodiscard]] amdgpu_instr_backend::Result<ParsedCodeObject>
parseCodeObject(const CodeObjectParseRequest &request);

[[nodiscard]] CodeObjectParseRequest
parseRequestFromRewriteRequest(const RewriteRequest &request);

} // namespace amdgpu_rewrite_core

#endif // AMDGPU_REWRITE_CORE_CODE_OBJECT_MODEL_H
