#pragma once

#include "FieldGroups.h"

#include <string>
#include <string_view>

namespace VNX {

inline constexpr std::string_view kFieldGroupsPropertyModel = "fieldGroupsV1";

bool ReadFieldGroupsJson(const std::string& text,
                           DefinitionFieldGroups& groups,
                           std::string& error);
bool WriteFieldGroupsJson(const DefinitionFieldGroups& groups,
                            std::string& text,
                            std::string& error);
bool ValidateFieldGroupsPropertyModelMarkerJson(const std::string& text,
                                                 std::string& error);
bool ValidateOccurrenceFieldGroupExclusionJson(const std::string& text,
                                                std::string& error);

}
