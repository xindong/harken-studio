#include "FilterAuthoring.h"

#include "Components/VMixNode.h"
#include "ProjectTypes.h"

namespace harken::studio {
namespace FilterAuthoring {

bool isField(const QString& field)
{
    return field == QLatin1String("filter") || FilterSemantics::fieldInfo(field);
}

FilterState effectiveState(const ProjectItem& item, PlatformScope scope)
{
    const auto* mix = item.comp<VMixNode>();
    if (!mix) return {};
    if (const auto* overlay = scope ? item.platformOverrides.find(*scope) : nullptr; overlay && !overlay->filter.isEmpty()) {
        const auto state = FilterSemantics::fromJson(overlay->filter);
        Q_ASSERT(state);
        return *state;
    }
    return mix->filter;
}

std::optional<PlayablePropertyStateV1> readProperty(
    const ProjectItem& item, const QString& field, PlatformScope scope, bool overlayOnly)
{
    const auto* mix = item.comp<VMixNode>();
    if (!mix || !isField(field)) return std::nullopt;
    const auto* descriptor = FilterSemantics::fieldInfo(field);
    const auto* overlay = scope ? item.platformOverrides.find(*scope) : nullptr;
    if (scope && overlayOnly) {
        if (!overlay) return PlayablePropertyStateV1{false, {}};
        if (!descriptor)
            return PlayablePropertyStateV1{!overlay->filter.isEmpty(), overlay->filter};
        const auto section = overlay->filter.value(QLatin1String(FilterSemantics::sectionKey(descriptor->kind))).toObject();
        const auto value = section.value(QLatin1String(FilterSemantics::parameterKey(descriptor->parameter)));
        return PlayablePropertyStateV1{!value.isUndefined(), value};
    }
    const auto state = effectiveState(item, scope);
    return PlayablePropertyStateV1{true, descriptor
        ? QJsonValue(FilterSemantics::value(state, *descriptor))
        : QJsonValue(FilterSemantics::toJson(state))};
}

bool restoreProperty(ProjectItem& item, const QString& field,
                     const PlayablePropertyStateV1& state, PlatformScope scope)
{
    auto* mix = item.comp<VMixNode>();
    if (!mix || !isField(field) || (scope && *scope == ProjectPlatformId{})) return false;
    const auto* descriptor = FilterSemantics::fieldInfo(field);
    if (!scope) {
        if (descriptor) {
            if (state.present && !state.value.isDouble()) return false;
            return FilterSemantics::setValue(mix->filter, *descriptor,
                state.present ? state.value.toDouble() : descriptor->defaultValue);
        }
        if (!state.present) {
            mix->filter = {};
            return true;
        }
        if (!state.value.isObject()) return false;
        const auto parsed = FilterSemantics::fromJson(state.value.toObject());
        if (!parsed) return false;
        mix->filter = *parsed;
        return true;
    }
    if (!state.present) {
        if (descriptor) return false;
        if (auto* overlay = item.platformOverrides.find(*scope)) {
            overlay->filter = {};
            item.platformOverrides.eraseEmpty(*scope);
        }
        return true;
    }
    FilterState next;
    if (descriptor) {
        next = effectiveState(item, scope);
        if (!state.value.isDouble() || !FilterSemantics::setValue(next, *descriptor, state.value.toDouble()))
            return false;
    } else {
        const auto parsed = state.value.isObject() ? FilterSemantics::fromJson(state.value.toObject()) : std::nullopt;
        if (!parsed) return false;
        next = *parsed;
    }
    item.platformOverrides.ensure(*scope)->filter = FilterSemantics::toJson(next);
    return true;
}

bool setOverride(ProjectItem& item, const QString& field,
                 const QJsonValue& value, PlatformScope scope)
{
    if (field == QLatin1String("filter")
        && (!value.isObject() || !FilterSemantics::fromJson(value.toObject()))) return false;
    return restoreProperty(item, field, {true, value}, scope);
}

bool clearOverride(ProjectItem& item, const QString& field, PlatformScope scope)
{
    return restoreProperty(item, field, {false, {}}, scope);
}

}
}
