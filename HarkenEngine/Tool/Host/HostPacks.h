#ifndef HARKEN_HOST_HOST_PACKS_H
#define HARKEN_HOST_HOST_PACKS_H

#include <harken/Harken.h>
#include <harken/HarkenDevelopment.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace harkenhost {

struct HostPack {
    HostPack() = default;
    HostPack(const HostPack&) = delete;
    HostPack& operator=(const HostPack&) = delete;
    std::vector<std::uint8_t> bytes;
    VnPackInspectReportV1 facts{};

    VnPackByteSpan Span() const { return {sizeof(VnPackByteSpan), 0, bytes.data(), bytes.size()}; }

    static VnResult Provide(void* context, std::uint64_t id, const char*, std::uint32_t,
                            std::uint64_t, VnPackByteSpan* out) {
        const auto& input = *static_cast<const HostPack*>(context);
        if (id != input.facts.packStableId) return VN_CONTENT_NOT_LOADED;
        *out = input.Span();
        return VN_OK;
    }
    static void Release(void*, std::uint64_t, VnPackByteSpan) {}
};

inline VnResult CaptureHostPack(const std::filesystem::path& path, const HostPack* init,
                                VnObjectId rootActionId, HostPack* out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return VN_CONTENT_NOT_LOADED;
    out->bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (!stream.good() && !stream.eof()) return VN_PACK_PROVIDER_FAILED;
    VnPackInspectDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.pack = out->Span();
    desc.init = init ? init->Span() : VnPackByteSpan{sizeof(VnPackByteSpan), 0, nullptr, 0};
    desc.rootActionId = rootActionId;
    desc.maxInputBytes = out->bytes.size() + desc.init.byteCount + 1024;
    out->facts = {};
    out->facts.structSize = sizeof(out->facts);
    return Vn_InspectCanonicalPackV1(&desc, &out->facts);
}

inline VnResult ActivateHostPacks(HostPack& init, HostPack& content,
                                  VnCanonicalPackSetActivationReportV1* outReport = nullptr) {
    VnCanonicalPackSetMemberV2 members[2]{};
    VnCanonicalPackSetMemberResultV1 results[2]{};
    HostPack* inputs[]{&init, &content};
    for (std::size_t i = 0; i < 2; ++i) {
        auto& member = members[i];
        member.structSize = sizeof(member);
        member.operation = VN_PACK_MEMBER_CONTENT;
        member.source.structSize = sizeof(member.source);
        member.source.flags = VN_CANONICAL_PACK_SET_MEMBER_PROVIDER;
        member.source.packStableId = inputs[i]->facts.packStableId;
        member.source.packKind = inputs[i]->facts.packKind;
        member.source.providerUserData = inputs[i];
        member.source.provide = HostPack::Provide;
        member.source.release = HostPack::Release;
        results[i].structSize = sizeof(results[i]);
    }
    VnCanonicalPackSetActivationDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.setRevision = 1;
    desc.memberCount = 2;
    desc.memberStructSize = sizeof(members[0]);
    desc.members = members;
    desc.outputMemberCapacity = 2;
    desc.outputMemberStructSize = sizeof(results[0]);
    desc.outputMembers = results;
    VnCanonicalPackSetActivationReportV1 report{};
    report.structSize = sizeof(report);
    const auto result = Vn_ActivateCanonicalPackSetV2(&desc, &report);
    if (outReport) *outReport = report;
    return result;
}

} // namespace harkenhost

#endif
