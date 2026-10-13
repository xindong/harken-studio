#ifndef HARKEN_API_HARKEN_VARIABLES_H
#define HARKEN_API_HARKEN_VARIABLES_H

#include <harken/Harken.h>
#include <harken/generated/HarkenVariables.g.h>

#ifdef __cplusplus

#include <concepts>
#include <cstdio>

namespace harken {

using VariableCatalogWarning = void (*)(std::uint64_t live, std::uint64_t generated);

inline void WarnVariableCatalogMismatch(std::uint64_t live,
                                       std::uint64_t generated) {
#ifndef NDEBUG
    std::fprintf(stderr, "Harken Variable SDK catalog stale: live %llu generated %llu\n",
                 static_cast<unsigned long long>(live),
                 static_cast<unsigned long long>(generated));
#else
    (void)live;
    (void)generated;
#endif
}

// Default arguments bind the caller's generated catalog, never the library's.
inline VnResult GuardTypedVariableCatalog(
    std::uint64_t execution = generated::kVariableExecutionCatalogFingerprint,
    std::uint64_t display = generated::kVariableCatalogFingerprint,
    VariableCatalogWarning warning = WarnVariableCatalogMismatch) noexcept {
    VnVariableCatalogInfoV1 live{};
    live.structSize = sizeof(live);
    const VnResult result = ::Vn_QueryVariableCatalog(&live);
    if (result != VN_OK) return result;
    if (live.executionCatalogFingerprint != execution)
        return VN_VARIABLE_CATALOG_MISMATCH;
    // Execution-only publications report zero: their display catalog is unavailable.
    if (live.sdkCatalogFingerprint != 0 && display != 0 &&
        live.sdkCatalogFingerprint != display && warning != nullptr)
        warning(live.sdkCatalogFingerprint, display);
    return VN_OK;
}

namespace variables {

inline VnResult SetValue(VnVariableId variable, VnObjectId label,
                        VnEmitterHandle emitter = 0) {
    const VnResult guard = GuardTypedVariableCatalog();
    if (guard != VN_OK) return guard;
    return ::Vn_SetValueByLabelId(variable, label, emitter);
}

inline VnResult SetValue(VnVariableId variable, const char* name,
                        VnEmitterHandle emitter = 0) {
    const VnResult guard = GuardTypedVariableCatalog();
    if (guard != VN_OK) return guard;
    return ::Vn_SetValueByLabelName(variable, name, emitter);
}

inline VnResult SetValue(const char* name, float value, VnEmitterHandle emitter = 0) {
    return emitter == 0 ? ::Vn_SetValueByName(name, value)
                        : ::Vn_SetEmitterValueByName(emitter, name, value);
}

inline VnResult Resolve(const char* name, VnVariableId* outVariableId) {
    if (outVariableId == nullptr) return VN_INVALID_ARG;
    *outVariableId = {};
    VnPostActionByNameDesc desc{};
    desc.structSize = sizeof(desc);
    desc.utf8Name = name;
    VnVariableResolved resolved{};
    resolved.structSize = sizeof(resolved);
    const auto result = ::Vn_ResolveVariableByName(&desc, &resolved);
    if (result == VN_OK) *outVariableId = resolved.variableId;
    return result;
}

inline VnResult SetValue(VnVariableId variable, float value,
                         VnEmitterHandle emitter = 0) {
    const VnResult guard = GuardTypedVariableCatalog();
    if (guard != VN_OK) return guard;
    return emitter == 0 ? ::Vn_SetValue(variable, value)
                        : ::Vn_SetEmitterValue(emitter, variable, value);
}

inline VnResult SetValue(const char* variableName, VnObjectId label,
                        VnEmitterHandle emitter = 0) {
    VnVariableId variable{};
    const VnResult result = Resolve(variableName, &variable);
    return result == VN_OK ? ::Vn_SetValueByLabelId(variable, label, emitter) : result;
}

inline VnResult SetValue(const char* variableName, const char* labelName,
                        VnEmitterHandle emitter = 0) {
    VnVariableId variable{};
    const VnResult result = Resolve(variableName, &variable);
    return result == VN_OK ? ::Vn_SetValueByLabelName(variable, labelName, emitter) : result;
}

template <std::integral T>
inline VnResult SetValue(VnVariableId variable, T value, VnEmitterHandle emitter = 0) {
    return SetValue(variable, static_cast<float>(value), emitter);
}

template <std::integral T>
inline VnResult SetValue(const char* name, T value, VnEmitterHandle emitter = 0) {
    return SetValue(name, static_cast<float>(value), emitter);
}

[[deprecated("Use SetValue(variable, value, optional emitter)")]]
inline VnResult SetValues(const VnVariableBatchDescV1* desc,
                          VnVariableTicketV1* outTicket) {
    const VnResult guard = GuardTypedVariableCatalog();
    if (guard != VN_OK) {
        return guard;
    }
    return ::Vn_SetValues(desc, outTicket);
}

[[deprecated("Use SetValue(variable, value, emitter)")]]
inline VnResult SetEmitterValues(VnEmitterHandle emitter,
                                 const VnVariableBatchDescV1* desc,
                                 VnVariableTicketV1* outTicket) {
    const VnResult guard = GuardTypedVariableCatalog();
    if (guard != VN_OK) {
        return guard;
    }
    return ::Vn_SetEmitterValues(emitter, desc, outTicket);
}

[[deprecated("Use SetValue with the Variable default value")]]
inline VnResult ResetValues(const VnVariableBatchDescV1* desc,
                            VnVariableTicketV1* outTicket) {
    const VnResult guard = GuardTypedVariableCatalog();
    if (guard != VN_OK) {
        return guard;
    }
    return ::Vn_ResetValues(desc, outTicket);
}

inline VnResult QueryVariable(const VnVariableQueryDescV1* desc,
                              VnVariableQueryResultV1* out) {
    const VnResult guard = GuardTypedVariableCatalog();
    if (guard != VN_OK) {
        return guard;
    }
    return ::Vn_QueryVariable(desc, out);
}

inline VnResult QueryVariableTicket(const VnVariableTicketQueryDescV1* desc,
                                    VnVariableTicketQueryResultV1* out) {
    const VnResult guard = GuardTypedVariableCatalog();
    if (guard != VN_OK) {
        return guard;
    }
    return ::Vn_QueryVariableTicket(desc, out);
}

} // namespace variables

} // namespace harken

#endif

#endif
