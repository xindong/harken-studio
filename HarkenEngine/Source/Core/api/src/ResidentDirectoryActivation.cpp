#include <harken/Harken.h>

#include <algorithm>
#include <limits>
#include <numeric>
#include <tuple>
#include <vector>

namespace {
struct Activation {
    const VnResidentDirectoryActivationV1& request;
    VnCanonicalPackSetActivationReportV1 report{};
};

auto Identity(const VnObjectId& id) {
    return std::make_tuple(id.high, id.low);
}

VnResult Activate(void* context, const VnResidentPackRowV1* input, uint32_t count) {
    auto& state = *static_cast<Activation*>(context);
    const auto& request = state.request;
    std::vector<uint32_t> order(count);
    std::iota(order.begin(), order.end(), 0u);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& expected = request.rows[i];
        const auto& facts = input[i].facts;
        if (Identity(facts.projectId) != Identity(request.projectId)) return VN_BAD_PACK;
        if (expected.packStableId || expected.rootActionId.high || expected.rootActionId.low) {
            if (expected.packStableId != facts.packStableId ||
                Identity(expected.rootActionId) != Identity(facts.rootActionId)) return VN_CAPABILITY_UNAVAILABLE;
        }
    }
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return Identity(input[a].facts.rootActionId) < Identity(input[b].facts.rootActionId);
    });
    for (uint32_t i = 1; i < count; ++i) {
        if (Identity(input[order[i]].facts.rootActionId) == Identity(input[order[i - 1]].facts.rootActionId))
            return VN_CAPABILITY_UNAVAILABLE;
    }
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return input[a].facts.packStableId < input[b].facts.packStableId;
    });
    std::vector<VnCanonicalPackSetMemberV2> members(count + 1);
    std::vector<VnCanonicalPackSetMemberResultV1> results(count + 1);
    members[0].structSize = sizeof(members[0]);
    members[0].operation = VN_PACK_MEMBER_KEEP;
    auto& init = members[0].source;
    init.structSize = sizeof(init);
    init.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    init.packStableId = request.initStableId;
    init.expectedHandle = request.initHandle;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& row = input[order[i]];
        if (row.facts.packStableId == init.packStableId ||
            (i && input[order[i - 1]].facts.packStableId == row.facts.packStableId))
            return VN_CAPABILITY_UNAVAILABLE;
        members[i + 1] = {sizeof(members[0]), VN_PACK_MEMBER_CONTENT, row.provider};
    }
    for (auto& row : results) row.structSize = sizeof(row);
    VnCanonicalPackSetActivationDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.expectedCurrentInitHandle = request.initHandle;
    desc.baseSetRevision = request.baseSetRevision;
    desc.setRevision = request.baseSetRevision + 1;
    desc.memberCount = desc.outputMemberCapacity = count + 1;
    desc.memberStructSize = sizeof(members[0]);
    desc.outputMemberStructSize = sizeof(results[0]);
    desc.members = members.data();
    desc.outputMembers = results.data();
    state.report.structSize = sizeof(state.report);
    const auto result = Vn_ActivateCanonicalPackSetV2(&desc, &state.report);
    if (result != VN_OK) return result;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& row = input[order[i]];
        request.rows[order[i]] = {row.facts.packStableId, row.facts.rootActionId,
            row.facts.contentHash, row.byteCount, row.facts.executionFingerprint, results[i + 1].handle};
    }
    return VN_OK;
}
}

extern "C" VnResult Vn_ActivateResidentPackDirectoryV1(
    const VnResidentDirectoryActivationV1* desc, VnCanonicalPackSetActivationReportV1* outReport) {
    if (!desc || desc->structSize != sizeof(*desc) || !outReport || outReport->structSize != sizeof(*outReport) ||
        !desc->rows || !desc->initStableId || !desc->initHandle || (!desc->projectId.high && !desc->projectId.low) ||
        desc->baseSetRevision == UINT64_MAX || desc->directory.memberCount == UINT32_MAX ||
        desc->rowCapacity < desc->directory.memberCount || desc->directory.visit || desc->directory.userData)
        return VN_INVALID_ARG;
    Activation state{*desc};
    auto visit = desc->directory;
    visit.visit = Activate;
    visit.userData = &state;
    const auto result = Vn_VisitResidentPackDirectoryV1(&visit);
    if (result == VN_OK) *outReport = state.report;
    return result;
}
