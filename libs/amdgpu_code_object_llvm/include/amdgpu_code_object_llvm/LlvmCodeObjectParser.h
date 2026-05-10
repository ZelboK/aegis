//===-- amdgpu_code_object_llvm/LlvmCodeObjectParser.h ----------*- C++ -*-===//

#ifndef AMDGPU_CODE_OBJECT_LLVM_LLVM_CODE_OBJECT_PARSER_H
#define AMDGPU_CODE_OBJECT_LLVM_LLVM_CODE_OBJECT_PARSER_H

#include "amdgpu_code_object/CodeObject.h"

#include <memory>

namespace amdgpu_code_object_llvm {

[[nodiscard]] std::unique_ptr<amdgpu_code_object::CodeObjectParser>
createLlvmCodeObjectParser();

} // namespace amdgpu_code_object_llvm

#endif // AMDGPU_CODE_OBJECT_LLVM_LLVM_CODE_OBJECT_PARSER_H
