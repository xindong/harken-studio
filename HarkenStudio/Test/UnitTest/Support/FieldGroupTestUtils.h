#pragma once

#include "Core/FieldGroups.h"

#include <array>
#include <string_view>

namespace HarkenTest {
namespace FieldGroupPlatformIds {
inline constexpr harkenprojectschema::AuthoringGuid Windows{UINT64_C(0x7e69e0e28d1552df), UINT64_C(0xa609ab96fff38750)};
inline constexpr harkenprojectschema::AuthoringGuid macOS{UINT64_C(0xa20e7261208a5332), UINT64_C(0xb0018cc1a2d81428)};
inline constexpr harkenprojectschema::AuthoringGuid Linux{UINT64_C(0x3d58f8d1ec2a5f83), UINT64_C(0x94361ee7ca758210)};
inline constexpr harkenprojectschema::AuthoringGuid Android{UINT64_C(0x97cb29eb309051cf), UINT64_C(0x90ee15f88ff671c5)};
inline constexpr harkenprojectschema::AuthoringGuid iOS{UINT64_C(0x19587786cb985146), UINT64_C(0x814a088cee6f47c9)};
inline constexpr harkenprojectschema::AuthoringGuid Universal{};
}

inline constexpr std::array fieldGroupPlatforms{
    FieldGroupPlatformIds::Windows, FieldGroupPlatformIds::macOS, FieldGroupPlatformIds::Linux, FieldGroupPlatformIds::Android, FieldGroupPlatformIds::iOS};

inline std::string_view fieldGroupPlatformIdString(harkenprojectschema::AuthoringGuid id)
{
    if (id == FieldGroupPlatformIds::Windows) return "7e69e0e2-8d15-52df-a609-ab96fff38750";
    if (id == FieldGroupPlatformIds::macOS) return "a20e7261-208a-5332-b001-8cc1a2d81428";
    if (id == FieldGroupPlatformIds::Linux) return "3d58f8d1-ec2a-5f83-9436-1ee7ca758210";
    if (id == FieldGroupPlatformIds::Android) return "97cb29eb-3090-51cf-90ee-15f88ff671c5";
    if (id == FieldGroupPlatformIds::iOS) return "19587786-cb98-5146-814a-088cee6f47c9";
    return "00000000-0000-0000-0000-000000000000";
}

}
