//===-- aegis/hsa_intercept/HsaTypes.h --------------------------*- C++ -*-===//

#ifndef AEGIS_HSA_INTERCEPT_HSA_TYPES_H
#define AEGIS_HSA_INTERCEPT_HSA_TYPES_H

#include <cstdint>

namespace aegis::hsa_intercept {

/// Opaque HSA queue pointer. Public headers intentionally avoid HSA headers.
using QueueHandle = void*;

/// Opaque HSA executable handle.
using ExecutableHandle = uint64_t;

/// Opaque HSA agent handle.
using AgentHandle = uint64_t;

/// Opaque HSA executable symbol handle.
using SymbolHandle = uint64_t;

/// Opaque HSA loaded code object handle.
using LoadedCodeObjectHandle = uint64_t;

} // namespace aegis::hsa_intercept

#endif // AEGIS_HSA_INTERCEPT_HSA_TYPES_H
