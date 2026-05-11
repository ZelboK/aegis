//===-- amdgpu_instr_backend_llvm/LlvmBackend.h -----------------*- C++ -*-===//

#ifndef AMDGPU_INSTR_BACKEND_LLVM_LLVM_BACKEND_H
#define AMDGPU_INSTR_BACKEND_LLVM_LLVM_BACKEND_H

#include "amdgpu_instr_backend/Backend.h"
#include "amdgpu_instr_backend/Result.h"

#include <memory>
#include <string>

namespace amdgpu_instr_backend_llvm {

struct LlvmBackendOptions {
  std::string CPU = "gfx950";
  std::string Features = "+wavefrontsize64";
  std::string TargetTriple = "amdgcn-amd-amdhsa";
};

[[nodiscard]] amdgpu_instr_backend::Result<
    std::unique_ptr<amdgpu_instr_backend::InstructionBackend>>
createLlvmBackend(LlvmBackendOptions Options = {});

} // namespace amdgpu_instr_backend_llvm

#endif // AMDGPU_INSTR_BACKEND_LLVM_LLVM_BACKEND_H
