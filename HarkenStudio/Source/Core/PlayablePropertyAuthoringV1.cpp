#include "Core/Components/VSelection.h"
#include "PlayablePropertyAuthoringV1.h"

#include "ComponentCatalog.h"
#include "ItemTypeCatalog.h"
#include "PlacementContext.h"
#include "PlaybackReferenceValidation.h"
#include "ProjectTypes.h"
#include "ReferenceSlotResolution.h"
#include "Components/VTrack.h"
#include "Components/VSpatial.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace VNX {
namespace {

using harkenplayableproperties::kPlayablePropertyRegistryV1;
using harkenplayableproperties::PlayablePropertyCapabilityRoleV1;
using harkenplayableproperties::PlayablePropertyDefaultV1;
using harkenplayableproperties::PlayablePropertyWireV1;
using harkenplayableproperties::studio::JsonFieldV1;

constexpr auto kEditableFields = [] {
    constexpr std::array<uint8_t, 13> ids{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 14, 17};
    std::array<JsonFieldV1, ids.size()> fields{};
    for (size_t index = 0; index < ids.size(); ++index)
        for (const auto& field : harkenplayableproperties::studio::kJsonFieldsV1)
            if (field.id == ids[index]) fields[index] = field;
    return fields;
}();

constexpr auto kAuthoredFields = [] {
    std::array<JsonFieldV1, kEditableFields.size() - 1> fields{};
    std::copy_n(kEditableFields.begin(), fields.size(), fields.begin());
    return fields;
}();

static_assert([] {
    for (const auto& field : kAuthoredFields)
        if (field.id == 0) return false;
    return true;
}());

constexpr uint64_t kAuthoredPresenceMask = [] {
    uint64_t mask = 0;
    for (const auto& field : kAuthoredFields) mask |= field.presentBit;
    return mask;
}();

void setError(QString* error, const QString& message)
{
    if (error) *error = message;
}

constexpr char kPlatformJsonKey[] = "platformId";

bool parseOverlayPlatform(const QJsonValue& value,
                          ProjectPlatformId* platform,
                          QString* error)
{
    if (!platform) return false;
    if (!value.isString()) {
        setError(error, QStringLiteral("platformOverrides platform must be a string"));
        return false;
    }
    const QString name = value.toString();
    const auto parsed = ProjectPlatformSemantics::parseId(name);
    if (!parsed) {
        setError(error, QStringLiteral("platformOverrides platform is invalid"));
        return false;
    }
    *platform = *parsed;
    return true;
}

std::optional<PlayablePropertyCapabilityRoleV1> capabilityRoleFor(uint8_t id)
{
    for (const auto& meta : kPlayablePropertyRegistryV1)
        if (static_cast<uint8_t>(meta.id) == id) return meta.capabilityRole;
    return std::nullopt;
}

IComponent* componentFor(ProjectItem& item, const JsonFieldV1& field)
{
    const auto type = ComponentCatalog::parseComponentName(
        QString::fromLatin1(field.component));
    if (!type) return nullptr;
    IComponent* result = nullptr;
    item.components.forEach([&](IComponent& component) {
        if (component.getType() == *type) result = &component;
    });
    return result;
}

const IComponent* componentFor(const ProjectItem& item, const JsonFieldV1& field)
{
    return componentFor(const_cast<ProjectItem&>(item), field);
}

QVariant canonicalValue(const JsonFieldV1& field)
{
    if (field.canonicalDefault == PlayablePropertyDefaultV1::PositiveZero)
        return QVariant(0.0f);
    if (field.canonicalDefault == PlayablePropertyDefaultV1::One)
        return QVariant(1.0f);
    if (field.canonicalDefault == PlayablePropertyDefaultV1::IntegerOne)
        return QVariant::fromValue(static_cast<uint32_t>(1));
    if (field.canonicalDefault == PlayablePropertyDefaultV1::Empty)
        return QVariant(QVariantList{});
    if (field.wire == PlayablePropertyWireV1::U8)
        return QVariant(false);
    if (field.wire == PlayablePropertyWireV1::U32)
        return QVariant::fromValue(static_cast<uint32_t>(0));
    if (field.wire == PlayablePropertyWireV1::ObjectId)
        return QVariant::fromValue(harkenstudio::ObjectId{});
    return QVariant(0.0f);
}

std::optional<QVariant> valueFromJson(const QJsonValue& value,
                                      const JsonFieldV1& field)
{
    if (field.wire == PlayablePropertyWireV1::F32) {
        if (!value.isDouble()) return std::nullopt;
        const double number = value.toDouble();
        if (PlayablePropertyAuthoringV1::isComponentStored(field.id)
            && (number < 0.0 || number > 1.0)) return std::nullopt;
        if (!std::isfinite(number)
            || number < -std::numeric_limits<float>::max()
            || number > std::numeric_limits<float>::max())
            return std::nullopt;
        return QVariant(static_cast<float>(number));
    }
    if (field.wire == PlayablePropertyWireV1::U8) {
        if (!value.isBool()) return std::nullopt;
        return QVariant(value.toBool());
    }
    if (field.wire == PlayablePropertyWireV1::U32) {
        if (!value.isDouble()) return std::nullopt;
        const double number = value.toDouble(-1.0);
        if (!std::isfinite(number) || std::trunc(number) != number
            || number < 0.0
            || number > static_cast<double>(std::numeric_limits<uint32_t>::max()))
            return std::nullopt;
        return QVariant::fromValue(static_cast<uint32_t>(number));
    }
    if (field.wire == PlayablePropertyWireV1::ObjectId) {
        harkenstudio::ObjectId id{};
        if (!parseObjectIdJson(value, &id)) return std::nullopt;
        return QVariant::fromValue(id);
    }
    if (field.wire == PlayablePropertyWireV1::RowSpan && value.isArray())
        return QVariant(value.toArray().toVariantList());
    return std::nullopt;
}

QJsonValue valueToJson(const QVariant& value, const JsonFieldV1& field)
{
    if (field.wire == PlayablePropertyWireV1::F32)
        return QJsonValue(static_cast<double>(value.toFloat()));
    if (field.wire == PlayablePropertyWireV1::U8)
        return QJsonValue(value.toBool());
    if (field.wire == PlayablePropertyWireV1::U32)
        return QJsonValue(static_cast<qint64>(value.toUInt()));
    if (field.wire == PlayablePropertyWireV1::ObjectId)
        return QJsonValue(objectIdString(value.value<harkenstudio::ObjectId>()));
    return QJsonArray::fromVariantList(value.toList());
}

PlayablePropertyOccurrenceValueV1 occurrenceValue(const QVariant& value,
                                                   const JsonFieldV1& field)
{
    if (field.wire == PlayablePropertyWireV1::F32) return value.toFloat();
    if (field.wire == PlayablePropertyWireV1::U8) return value.toBool();
    if (field.wire == PlayablePropertyWireV1::U32) return value.toUInt();
    if (field.wire == PlayablePropertyWireV1::ObjectId)
        return value.value<harkenstudio::ObjectId>();
    return QJsonArray::fromVariantList(value.toList());
}

bool occurrenceValueMatchesWire(const PlayablePropertyOccurrenceValueV1& value,
                                PlayablePropertyWireV1 wire)
{
    if (wire == PlayablePropertyWireV1::F32)
        return std::holds_alternative<float>(value);
    if (wire == PlayablePropertyWireV1::U8)
        return std::holds_alternative<bool>(value);
    if (wire == PlayablePropertyWireV1::U32)
        return std::holds_alternative<uint32_t>(value);
    if (wire == PlayablePropertyWireV1::ObjectId)
        return std::holds_alternative<harkenstudio::ObjectId>(value);
    return std::holds_alternative<QJsonArray>(value);
}

std::optional<QJsonValue> occurrenceValueToJson(
    const PlayablePropertyOccurrenceValueV1& value,
    PlayablePropertyWireV1 wire)
{
    if (!occurrenceValueMatchesWire(value, wire)) return std::nullopt;
    if (wire == PlayablePropertyWireV1::F32) {
        const float number = std::get<float>(value);
        if (!std::isfinite(number)) return std::nullopt;
        return QJsonValue(static_cast<double>(number));
    }
    if (wire == PlayablePropertyWireV1::U8)
        return QJsonValue(std::get<bool>(value));
    if (wire == PlayablePropertyWireV1::U32)
        return QJsonValue(static_cast<qint64>(std::get<uint32_t>(value)));
    if (wire == PlayablePropertyWireV1::ObjectId)
        return QJsonValue(objectIdString(std::get<harkenstudio::ObjectId>(value)));
    return QJsonValue(std::get<QJsonArray>(value));
}

bool readUnsigned(const QJsonValue& value, uint64_t maximum, uint64_t* out)
{
    if (!out || !value.isDouble()) return false;
    const double number = value.toDouble(-1.0);
    if (!std::isfinite(number) || std::trunc(number) != number
        || number < 0.0 || number > static_cast<double>(maximum))
        return false;
    *out = static_cast<uint64_t>(number);
    return true;
}

bool validateContainerOccurrenceLedgerShape(
    const SelectionTable& ledger,
    QString* error)
{
    if (ledger.occurrenceOrdinalHighWater > UINT64_C(0x100000000)) {
        setError(error, QStringLiteral("Container occurrence high-water is invalid"));
        return false;
    }
    QSet<uint32_t> ordinals;
    for (const auto& row : ledger.occurrenceRows()) {
        if (row.childGuid.isNull()
            || ordinals.contains(row.stableOccurrenceOrdinal)
            || static_cast<uint64_t>(row.stableOccurrenceOrdinal)
                >= ledger.occurrenceOrdinalHighWater) {
            setError(error, QStringLiteral("Container occurrence ledger shape is invalid"));
            return false;
        }
        if (row.weight < 0 || row.weight > 100) {
            setError(error, QStringLiteral("Selection weight is invalid"));
            return false;
        }
        ordinals.insert(row.stableOccurrenceOrdinal);
    }
    return true;
}

struct AuthoringLocusV1
{
    PlayablePropertyOccurrenceV1* occurrence = nullptr;
    PlatformOverrideLedgerV1* ledger = nullptr;
    bool valid = false;
};

AuthoringLocusV1 locusFor(ProjectItem& owner, const PlayablePropertyLocatorV1& locator, PlatformScope scope = std::nullopt)
{
    if (owner.guid.isNull() || owner.guid != locator.ownerGuid) return {};
    if (locator.kind == PlayablePropertyTargetKindV1::Definition)
        return {nullptr, &owner.platformOverrides, true};
    if (locator.kind == PlayablePropertyTargetKindV1::TrackClip) {
        if (owner.type != ProjectItemType::Track) return {};
        auto* timeline = owner.comp<VTrack>();
        if (!timeline) return {};
        for (Clip& clip : timeline->clips.items) {
            if (clip.clipGuid == locator.occurrenceGuid
                && clip.targetGuid == locator.targetGuid
                && clip.stableOccurrenceOrdinal == locator.stableOccurrenceOrdinal)
                return {&clip.playableProperties, &clip.platformOverrides, true};
        }
        return {};
    }
    if (locator.kind == PlayablePropertyTargetKindV1::ContainerChild) {
        if (owner.type != ProjectItemType::Container
            || locator.occurrenceGuid != locator.targetGuid)
            return {};
        for (const auto& row : owner.comp<VSelection>()->activeRows(scope)) {
            if (row.childGuid == locator.occurrenceGuid
                && row.stableOccurrenceOrdinal == locator.stableOccurrenceOrdinal)
                return {nullptr, nullptr, true};
        }
    }
    return {};
}

std::optional<PlayablePropertyStateV1> overlayStateIfPresent(
    const PlatformOverrideLedgerV1& ledger,
    ProjectPlatformId platform,
    uint8_t propertyId,
    const JsonFieldV1& field)
{
    const auto* row = ledger.find(platform);
    if (!row) return std::nullopt;
    const auto id = static_cast<harkenplayableproperties::PlayablePropertyIdV1>(propertyId);
    if (!row->properties.presence.Has(id)) return std::nullopt;
    const auto* value = row->properties.value(id);
    if (!value) return std::nullopt;
    const auto json = occurrenceValueToJson(*value, field.wire);
    if (!json) return std::nullopt;
    return PlayablePropertyStateV1{true, *json};
}

bool writeOverlaySlot(PlatformOverrideLedgerV1& ledger,
                      ProjectPlatformId platform,
                      uint8_t propertyId,
                      const QVariant& parsed,
                      const JsonFieldV1& field)
{
    auto* row = ledger.ensure(platform);
    if (!row) return false;
    const auto id = static_cast<harkenplayableproperties::PlayablePropertyIdV1>(propertyId);
    if (!row->properties.setValue(id, occurrenceValue(parsed, field)))
        return false;
    row->properties.presence.Set(id);
    return true;
}

bool clearOverlaySlot(PlatformOverrideLedgerV1& ledger,
                      ProjectPlatformId platform,
                      uint8_t propertyId,
                      const JsonFieldV1& field)
{
    auto* row = ledger.find(platform);
    if (!row) return true;
    const auto id = static_cast<harkenplayableproperties::PlayablePropertyIdV1>(propertyId);
    if (!row->properties.setValue(id, occurrenceValue(canonicalValue(field), field)))
        return false;
    row->properties.presence.Clear(id);
    ledger.eraseEmpty(platform);
    return true;
}

void appendDefinitionSource(std::optional<PlayablePropertySourceV1>* source,
                            ProjectItem& item,
                            uint8_t propertyId,
                            PlayablePropertySourceV1::Kind kind,
                            PlatformScope scope)
{
    if (!source) return;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
    if (!locator) return;
    const auto state = PlayablePropertyAuthoringV1::readProperty(
        item, *locator, propertyId, scope);
    if (state && state->present)
        *source = PlayablePropertySourceV1{*locator, &item, *state, kind};
}

void appendOccurrenceSource(std::optional<PlayablePropertySourceV1>* source,
                            ProjectItem& owner,
                            const PlayablePropertyLocatorV1& locator,
                            uint8_t propertyId,
                            PlatformScope scope)
{
    if (!source) return;
    const auto state = PlayablePropertyAuthoringV1::readProperty(
        owner, locator, propertyId, scope);
    if (state && state->present)
        *source = PlayablePropertySourceV1{
            locator, &owner, *state,
            PlayablePropertySourceV1::Kind::Placement};
}

bool isStructuralParentType(ProjectItemType type) noexcept
{
    return type == ProjectItemType::Layer
        || type == ProjectItemType::Container
        || type == ProjectItemType::Track
        || type == ProjectItemType::Piece;
}

void appendStructuralParents(std::optional<PlayablePropertySourceV1>* source,
                             ProjectItem& directRoot,
                             uint8_t propertyId,
                             PlatformScope scope)
{
    std::vector<ProjectItem*> parents;
    for (ProjectItem* item = directRoot.parent; item; item = item->parent)
        if (isStructuralParentType(item->type))
            parents.push_back(item);
    std::reverse(parents.begin(), parents.end());
    for (ProjectItem* parent : parents)
        appendDefinitionSource(
            source, *parent, propertyId,
            PlayablePropertySourceV1::Kind::Inherited,
            scope);
}
}

PlayablePropertyOccurrenceV1::PlayablePropertyOccurrenceV1()
{
    for (const JsonFieldV1& field : kAuthoredFields)
        values[field.id - 1] = occurrenceValue(canonicalValue(field), field);
}

bool PlayablePropertyOccurrenceV1::setValue(
    harkenplayableproperties::PlayablePropertyIdV1 id,
    PlayablePropertyOccurrenceValueV1 value)
{
    const auto raw = static_cast<uint8_t>(id);
    if (raw < 1 || raw > values.size()) return false;
    const auto* field = PlayablePropertyAuthoringV1::fieldById(raw);
    if (!field || !occurrenceValueMatchesWire(value, field->wire)) return false;
    values[raw - 1] = std::move(value);
    return true;
}

const PlayablePropertyOccurrenceValueV1* PlayablePropertyOccurrenceV1::value(
    harkenplayableproperties::PlayablePropertyIdV1 id) const noexcept
{
    const auto raw = static_cast<uint8_t>(id);
    return raw >= 1 && raw <= values.size()
        && PlayablePropertyAuthoringV1::fieldById(raw) ? &values[raw - 1] : nullptr;
}

bool PlayablePropertyOccurrenceV1::hasExactWireTypes() const noexcept
{
    for (const JsonFieldV1& field : kAuthoredFields)
        if (!occurrenceValueMatchesWire(values[field.id - 1], field.wire))
            return false;
    return true;
}

bool PlayablePropertyAuthoringV1::isDefinitionType(ProjectItemType type) noexcept
{
    return isFieldGroupDefinitionType(type) || type == ProjectItemType::Bus;
}

bool PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(ProjectItemType type) noexcept
{
    return type == ProjectItemType::Layer
        || type == ProjectItemType::Sound
        || type == ProjectItemType::Container
        || type == ProjectItemType::Track
        || type == ProjectItemType::Piece;
}

std::span<const PlayablePropertyAuthoringV1::Field>
PlayablePropertyAuthoringV1::fields() noexcept
{
    return kEditableFields;
}

bool PlayablePropertyAuthoringV1::isComponentStored(uint8_t propertyId) noexcept
{
    return propertyId == static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::SpatializationMix);
}

std::optional<PlayablePropertySourceV1>
PlayablePropertyAuthoringV1::resolveSpatializationMix(
    ProjectItem& subject,
    const std::function<ProjectItem*(const ProjectItem&)>& parentOf)
{
    QSet<const ProjectItem*> visited;
    for (auto* item = &subject; item && !visited.contains(item);
         item = parentOf ? parentOf(*item) : item->parent) {
        visited.insert(item);
        const auto* spatial = item->comp<VSpatial>();
        const auto locator = definitionLocator(*item);
        if (locator && spatial && spatial->spatializationMixPresent)
            return PlayablePropertySourceV1{
                *locator, item, {true, spatial->spatializationMix},
                item == &subject ? PlayablePropertySourceV1::Kind::Own
                                 : PlayablePropertySourceV1::Kind::Inherited};
    }
    return std::nullopt;
}

bool PlayablePropertyAuthoringV1::isSlotApplicable(ProjectItemType type,
                                                   uint8_t propertyId) noexcept
{
    if (!isDefinitionType(type) || !fieldById(propertyId)) return false;
    const auto role = capabilityRoleFor(propertyId);
    if (!role) return false;
    switch (*role) {
    case PlayablePropertyCapabilityRoleV1::Common:
    case PlayablePropertyCapabilityRoleV1::Send:
    case PlayablePropertyCapabilityRoleV1::Effect:
        return ItemTypeCatalog::hasMixingCapability(type);
    case PlayablePropertyCapabilityRoleV1::Route:
        if (propertyId == static_cast<uint8_t>(
                harkenplayableproperties::PlayablePropertyIdV1::MainOutputBusRef)
            && type == ProjectItemType::Bus)
            return false; // Bus main output is its tree parent.
        return ItemTypeCatalog::hasMixingCapability(type);
    case PlayablePropertyCapabilityRoleV1::Spatial:
        return ItemTypeCatalog::hasSpatialCapability(type);
    case PlayablePropertyCapabilityRoleV1::Playback:
        return ItemTypeCatalog::allowsLoop(type);
    case PlayablePropertyCapabilityRoleV1::Streaming:
        return ItemTypeCatalog::isContentObject(type);
    }
    return false;
}

PlayablePropertyValueRuleV1 PlayablePropertyAuthoringV1::valueRule(
    ProjectItemType type, uint8_t propertyId) noexcept
{
    if (type == ProjectItemType::Bus || !isSlotApplicable(type, propertyId))
        return PlayablePropertyValueRuleV1::Local;
    using Combine = harkenplayableproperties::PlayablePropertyCombineRoleV1;
    for (const auto& meta : kPlayablePropertyRegistryV1) {
        if (static_cast<uint8_t>(meta.id) != propertyId) continue;
        switch (meta.combineRole) {
        case Combine::DbSum:
        case Combine::CentSumClamp:
            return PlayablePropertyValueRuleV1::Add;
        case Combine::NearestExplicit:
            return PlayablePropertyValueRuleV1::Inherit;
        case Combine::SendResolution:
            return PlayablePropertyValueRuleV1::MergeByTarget;
        default:
            return PlayablePropertyValueRuleV1::Local;
        }
    }
    return PlayablePropertyValueRuleV1::Local;
}

bool PlayablePropertyAuthoringV1::isEditorExposed(uint8_t propertyId) noexcept
{
    // Streaming media delivery is not implemented end to end: the pack build
    // never emits a streamed container, and the runtime refuses a cut whose
    // streamingMode is non-zero. Exposing the slot would only offer a choice
    // that silently stops the object from playing.
    return fieldById(propertyId) && propertyId != static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::StreamingMode);
}

bool PlayablePropertyAuthoringV1::isInspectorVisible(uint8_t propertyId) noexcept
{
    const Field* field = fieldById(propertyId);
    return field
        && isEditorExposed(propertyId)
        && propertyId != static_cast<uint8_t>(
            harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain);
}

const PlayablePropertyAuthoringV1::Field*
PlayablePropertyAuthoringV1::fieldById(uint8_t id) noexcept
{
    for (const Field& field : kEditableFields)
        if (field.id == id) return &field;
    return nullptr;
}

const PlayablePropertyAuthoringV1::Field*
PlayablePropertyAuthoringV1::fieldByJsonKey(const QString& jsonKey) noexcept
{
    for (const Field& field : kAuthoredFields)
        if (jsonKey == QLatin1String(field.jsonKey)) return &field;
    return nullptr;
}

const PlayablePropertyAuthoringV1::Field*
PlayablePropertyAuthoringV1::fieldByName(const QString& name) noexcept
{
    for (const Field& field : kEditableFields) {
        if (name == QLatin1String(field.fieldName)
            || name == QLatin1String(field.jsonKey)
            || name == QLatin1String(field.storageField))
            return &field;
    }
    return nullptr;
}

std::optional<QJsonValue>
PlayablePropertyAuthoringV1::wireValueFromVariant(
    uint8_t propertyId, const QVariant& value)
{
    const Field* field = fieldById(propertyId);
    if (!field || !value.isValid()) return std::nullopt;
    QJsonValue json;
    if (field->wire == PlayablePropertyWireV1::ObjectId) {
        if (!value.canConvert<harkenstudio::ObjectId>()) return std::nullopt;
        json = objectIdString(value.value<harkenstudio::ObjectId>());
    } else if (field->wire == PlayablePropertyWireV1::RowSpan) {
        if (!value.canConvert<QVariantList>()) return std::nullopt;
        json = QJsonArray::fromVariantList(value.toList());
    } else {
        json = QJsonValue::fromVariant(value);
    }
    return valueFromJson(json, *field) ? std::optional<QJsonValue>(json)
                                       : std::nullopt;
}

std::optional<PlayablePropertyLocatorV1>
PlayablePropertyAuthoringV1::definitionLocator(const ProjectItem& item)
{
    if (!isDefinitionType(item.type) || item.guid.isNull()) return std::nullopt;
    PlayablePropertyLocatorV1 locator;
    locator.ownerGuid = item.guid;
    return locator;
}

std::optional<PlayablePropertyLocatorV1>
PlayablePropertyAuthoringV1::trackClipLocator(const ProjectItem& track,
                                               const QUuid& clipGuid)
{
    if (track.type != ProjectItemType::Track || track.guid.isNull()
        || clipGuid.isNull())
        return std::nullopt;
    const auto* timeline = track.comp<VTrack>();
    if (!timeline) return std::nullopt;
    for (const Clip& clip : timeline->clips.items) {
        if (clip.clipGuid != clipGuid) continue;
        return PlayablePropertyLocatorV1{
            PlayablePropertyTargetKindV1::TrackClip,
            track.guid,
            clip.clipGuid,
            clip.targetGuid,
            clip.stableOccurrenceOrdinal};
    }
    return std::nullopt;
}

std::optional<PlayablePropertyLocatorV1>
PlayablePropertyAuthoringV1::containerChildLocator(
    const ProjectItem& container, const QUuid& childGuid, PlatformScope scope)
{
    if (container.type != ProjectItemType::Container || container.guid.isNull()
        || childGuid.isNull())
        return std::nullopt;
    for (const auto& row : container.comp<VSelection>()->activeRows(scope)) {
        if (row.childGuid == childGuid)
            return PlayablePropertyLocatorV1{
                PlayablePropertyTargetKindV1::ContainerChild, container.guid,
                childGuid, childGuid, row.stableOccurrenceOrdinal};
    }
    return std::nullopt;
}

uint32_t PlayablePropertyAuthoringV1::containerChildOccurrenceCount(
    const ProjectItem& container, const QUuid& childGuid, PlatformScope scope)
{
    if (container.type != ProjectItemType::Container || childGuid.isNull())
        return 0;
    uint32_t count = 0;
    for (const auto& row : container.comp<VSelection>()->selections.table(scope).occurrenceRows()) {
        if (row.childGuid == childGuid)
            ++count;
    }
    return count;
}

std::optional<PlayablePropertyLocatorV1>
PlayablePropertyAuthoringV1::containerChildLocator(
    const ProjectItem& container, uint32_t stableOccurrenceOrdinal, PlatformScope scope)
{
    if (container.type != ProjectItemType::Container || container.guid.isNull())
        return std::nullopt;
    for (const auto& row : container.comp<VSelection>()->activeRows(scope)) {
        if (row.stableOccurrenceOrdinal == stableOccurrenceOrdinal)
            return PlayablePropertyLocatorV1{
                PlayablePropertyTargetKindV1::ContainerChild, container.guid,
                row.childGuid, row.childGuid, stableOccurrenceOrdinal};
    }
    return std::nullopt;
}

std::optional<PlayablePropertyLocatorV1>
PlayablePropertyAuthoringV1::placementLocator(
    const ProjectItem& subject,
    const PlacementContext& placement,
    const std::function<ProjectItem*(const QUuid&)>& resolveItem)
{
    if (!resolveItem || placement.empty()) return definitionLocator(subject);
    const auto scope = ReferenceSlotResolution::buildResolutionScope(
        &subject, placement, resolveItem);
    if (!scope.placementContextValid) return std::nullopt;
    const PlacementOccurrence& occurrence = placement.occurrences().front();
    if (occurrence.targetGuid != subject.guid) return std::nullopt;
    ProjectItem* track = resolveItem(occurrence.trackGuid);
    if (!track) return std::nullopt;
    const auto locator = trackClipLocator(*track, occurrence.clipGuid);
    if (!locator || locator->targetGuid != occurrence.targetGuid)
        return std::nullopt;
    return locator;
}

std::optional<PlayablePropertyStateV1>
PlayablePropertyAuthoringV1::readProperty(
    ProjectItem& owner,
    const PlayablePropertyLocatorV1& locator,
    uint8_t propertyId,
    PlatformScope scope)
{
    const Field* field = fieldById(propertyId);
    if (!field || owner.guid.isNull() || owner.guid != locator.ownerGuid)
        return std::nullopt;
    if (scope && *scope == ProjectPlatformId{})
        return std::nullopt;
    const auto locus = locusFor(owner, locator, scope);
    if (locator.kind != PlayablePropertyTargetKindV1::Definition) {
        if (!locus.valid) return std::nullopt;
        return PlayablePropertyStateV1{false, valueToJson(canonicalValue(*field), *field)};
    }
    if (!isSlotApplicable(owner.type, propertyId)) return std::nullopt;
    const IComponent* component = componentFor(owner, *field);
    if (!component) return std::nullopt;
    if (isComponentStored(propertyId)) {
        const auto* spatial = owner.comp<VSpatial>();
        return PlayablePropertyStateV1{
            spatial->spatializationMixPresent, spatial->spatializationMix};
    }
    if (scope && !(isFieldGroupDefinitionType(owner.type)
        && FieldGroupForProperty(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(propertyId)))) {
        if (!locus.ledger) return std::nullopt;
        if (const auto overlay = overlayStateIfPresent(
                *locus.ledger, *scope, propertyId, *field))
            return overlay;
    }
    return PlayablePropertyStateV1{
        (isFieldGroupDefinitionType(owner.type)
            && FieldGroupForProperty(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(propertyId)))
            || (owner.playablePropertyPresence.bits & field->presentBit) != 0,
        valueToJson(component->getField(QLatin1String(field->storageField)),
                    *field)};
}

std::optional<PlayablePropertyStateV1>
PlayablePropertyAuthoringV1::readPlatformOverlay(
    ProjectItem& owner,
    const PlayablePropertyLocatorV1& locator,
    uint8_t propertyId,
    ProjectPlatformId platform)
{
    const Field* field = fieldById(propertyId);
    if (!field || owner.guid.isNull() || owner.guid != locator.ownerGuid)
        return std::nullopt;
    if (platform == ProjectPlatformId{})
        return std::nullopt;
    if (isComponentStored(propertyId)) return PlayablePropertyStateV1{false, {}};
    if (locator.kind != PlayablePropertyTargetKindV1::Definition) {
        if (!locusFor(owner, locator, platform).valid) return std::nullopt;
        return PlayablePropertyStateV1{false, {}};
    }
    if (FieldGroupForProperty(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(propertyId))
        && isFieldGroupDefinitionType(owner.type))
        return PlayablePropertyStateV1{false, {}};
    const auto locus = locusFor(owner, locator, platform);
    if (!isSlotApplicable(owner.type, propertyId)) return std::nullopt;
    if (!componentFor(owner, *field)) return std::nullopt;
    if (!locus.ledger) return std::nullopt;
    if (const auto overlay = overlayStateIfPresent(
            *locus.ledger, platform, propertyId, *field))
        return overlay;
    return PlayablePropertyStateV1{false, {}};
}

bool PlayablePropertyAuthoringV1::setOverride(
    ProjectItem& owner,
    const PlayablePropertyLocatorV1& locator,
    uint8_t propertyId,
    const QJsonValue& value,
    PlatformScope scope)
{
    if (locator.kind != PlayablePropertyTargetKindV1::Definition) return false;
    if (FieldGroupForProperty(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(propertyId))
        && isFieldGroupDefinitionType(owner.type))
        return false;
    const Field* field = fieldById(propertyId);
    if (!field || owner.guid.isNull() || owner.guid != locator.ownerGuid)
        return false;
    if (scope && *scope == ProjectPlatformId{})
        return false;
    const auto parsed = valueFromJson(value, *field);
    if (!parsed) return false;
    if (!isSlotApplicable(owner.type, propertyId))
        return false;
    if (isComponentStored(propertyId)) {
        auto* spatial = owner.comp<VSpatial>();
        return !scope && spatial
            && spatial->setField(QStringLiteral("spatializationMix"), *parsed);
    }
    const auto locus = locusFor(owner, locator, scope);
    if (scope) {
        if (!componentFor(owner, *field)) return false;
        if (!locus.ledger) return false;
        return writeOverlaySlot(*locus.ledger, *scope, propertyId, *parsed, *field);
    }
    IComponent* component = componentFor(owner, *field);
    if (!component
        || !component->setField(QLatin1String(field->storageField), *parsed))
        return false;
    owner.playablePropertyPresence.Set(
        static_cast<harkenplayableproperties::PlayablePropertyIdV1>(propertyId));
    return true;
}

bool PlayablePropertyAuthoringV1::clearOverride(
    ProjectItem& owner,
    const PlayablePropertyLocatorV1& locator,
    uint8_t propertyId,
    PlatformScope scope)
{
    if (locator.kind != PlayablePropertyTargetKindV1::Definition) return false;
    if (FieldGroupForProperty(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(propertyId))
        && isFieldGroupDefinitionType(owner.type))
        return false;
    const Field* field = fieldById(propertyId);
    if (!field || owner.guid.isNull() || owner.guid != locator.ownerGuid)
        return false;
    if (scope && *scope == ProjectPlatformId{})
        return false;
    if (!isSlotApplicable(owner.type, propertyId))
        return false;
    if (isComponentStored(propertyId)) {
        auto* spatial = owner.comp<VSpatial>();
        if (scope || !spatial) return false;
        spatial->spatializationMix = 1.0f;
        spatial->spatializationMixPresent = false;
        return true;
    }
    const auto locus = locusFor(owner, locator, scope);
    if (scope) {
        if (!componentFor(owner, *field)) return false;
        if (!locus.ledger) return false;
        return clearOverlaySlot(*locus.ledger, *scope, propertyId, *field);
    }
    const QVariant canonical = canonicalValue(*field);
    IComponent* component = componentFor(owner, *field);
    if (!component
        || !component->setField(QLatin1String(field->storageField), canonical))
        return false;
    owner.playablePropertyPresence.Clear(
        static_cast<harkenplayableproperties::PlayablePropertyIdV1>(propertyId));
    return true;
}

bool PlayablePropertyAuthoringV1::restoreProperty(
    ProjectItem& owner,
    const PlayablePropertyLocatorV1& locator,
    uint8_t propertyId,
    const PlayablePropertyStateV1& state,
    PlatformScope scope)
{
    return state.present
        ? setOverride(owner, locator, propertyId, state.value, scope)
        : clearOverride(owner, locator, propertyId, scope);
}

std::optional<PlayablePropertySourceV1>
PlayablePropertyAuthoringV1::resolveSource(
    ProjectItem& subject,
    const PlayablePropertyLocatorV1& editedLocator,
    uint8_t propertyId,
    const PlacementContext* placement,
    const std::function<ProjectItem*(const QUuid&)>& resolveItem,
    PlatformScope scope)
{
    if (!fieldById(propertyId)) return std::nullopt;
    if (isComponentStored(propertyId)) return resolveSpatializationMix(subject);
    const bool inherits = valueRule(subject.type, propertyId)
        == PlayablePropertyValueRuleV1::Inherit;
    auto localSource = [&]() -> std::optional<PlayablePropertySourceV1> {
        ProjectItem* owner = editedLocator.ownerGuid == subject.guid
            ? &subject : resolveItem ? resolveItem(editedLocator.ownerGuid) : nullptr;
        if (!owner) return std::nullopt;
        const auto state = readProperty(*owner, editedLocator, propertyId, scope);
        if (!state || !state->present) return std::nullopt;
        return PlayablePropertySourceV1{
            editedLocator, owner, *state,
            editedLocator.kind == PlayablePropertyTargetKindV1::Definition
                ? PlayablePropertySourceV1::Kind::Own
                : PlayablePropertySourceV1::Kind::Placement};
    };
    if (editedLocator.kind == PlayablePropertyTargetKindV1::ContainerChild) {
        if (!resolveItem) return std::nullopt;
        ProjectItem* container = resolveItem(editedLocator.ownerGuid);
        ProjectItem* child = resolveItem(editedLocator.targetGuid);
        if (!container || !child || child->parent != container)
            return std::nullopt;
        if (!inherits) return localSource();
        std::optional<PlayablePropertySourceV1> source;
        appendStructuralParents(&source, *container, propertyId, scope);
        appendDefinitionSource(
            &source, *container, propertyId,
            PlayablePropertySourceV1::Kind::Placement,
            scope);
        appendOccurrenceSource(
            &source, *container, editedLocator, propertyId, scope);
        appendDefinitionSource(
            &source, *child, propertyId,
            PlayablePropertySourceV1::Kind::Own,
            scope);
        return source;
    }
    if (!placement || placement->empty()) {
        if (!inherits) return localSource();
        std::optional<PlayablePropertySourceV1> source;
        appendStructuralParents(&source, subject, propertyId, scope);
        appendDefinitionSource(
            &source, subject, propertyId,
            PlayablePropertySourceV1::Kind::Own,
            scope);
        return source;
    }

    if (!resolveItem) return std::nullopt;

    const auto exactEditedLocator = placementLocator(
        subject, *placement, resolveItem);
    if (!exactEditedLocator || *exactEditedLocator != editedLocator)
        return std::nullopt;

    const auto resolution = ReferenceSlotResolution::buildResolutionScope(
        &subject, *placement, resolveItem);
    if (!resolution.placementContextValid) return std::nullopt;
    const auto& occurrences = placement->occurrences();
    if (occurrences.empty()) return std::nullopt;
    ProjectItem* outerTrack = resolveItem(occurrences.back().trackGuid);
    ProjectItem* outerPiece = outerTrack ? outerTrack->parent : nullptr;
    if (!outerTrack || !outerPiece || outerPiece->type != ProjectItemType::Piece)
        return std::nullopt;
    if (!inherits) return localSource();

    std::optional<PlayablePropertySourceV1> source;
    appendStructuralParents(&source, *outerPiece, propertyId, scope);
    appendDefinitionSource(
        &source, *outerPiece, propertyId,
        PlayablePropertySourceV1::Kind::Placement,
        scope);
    for (auto it = occurrences.rbegin(); it != occurrences.rend(); ++it) {
        ProjectItem* track = resolveItem(it->trackGuid);
        ProjectItem* target = resolveItem(it->targetGuid);
        const auto locator = track ? trackClipLocator(*track, it->clipGuid)
                                   : std::nullopt;
        if (!track || !target || !locator
            || locator->targetGuid != it->targetGuid)
            return std::nullopt;
        appendDefinitionSource(
            &source, *track, propertyId,
            PlayablePropertySourceV1::Kind::Placement,
            scope);
        appendOccurrenceSource(
            &source, *track, *locator, propertyId, scope);
        appendDefinitionSource(
            &source, *target, propertyId,
            target == &subject ? PlayablePropertySourceV1::Kind::Own
                               : PlayablePropertySourceV1::Kind::Placement,
            scope);
    }
    return source;
}

QString PlayablePropertyAuthoringV1::encodePresence(uint64_t bits)
{
    return QStringLiteral("%1").arg(static_cast<qulonglong>(bits), 16, 16,
                                     QLatin1Char('0'));
}

std::optional<harkenplayableproperties::ChassisPresenceV1>
PlayablePropertyAuthoringV1::decodePresence(const QString& text)
{
    if (text.size() != 16) return std::nullopt;
    for (const QChar character : text) {
        const bool decimal = character >= QLatin1Char('0')
            && character <= QLatin1Char('9');
        const bool lowercaseHex = character >= QLatin1Char('a')
            && character <= QLatin1Char('f');
        if (!decimal && !lowercaseHex) return std::nullopt;
    }
    bool ok = false;
    const qulonglong bits = text.toULongLong(&ok, 16);
    harkenplayableproperties::ChassisPresenceV1 presence{
        static_cast<uint64_t>(bits)};
    if (!ok || (presence.bits & ~kAuthoredPresenceMask) != 0) return std::nullopt;
    return presence;
}

QJsonObject PlayablePropertyAuthoringV1::definitionToJson(const ProjectItem& item)
{
    if ((item.playablePropertyPresence.bits & ~kAuthoredPresenceMask) != 0)
        return {};
    if (isFieldGroupDefinitionType(item.type)
        && (item.playablePropertyPresence.bits & kFieldGroupPropertyMask) != 0)
        return {};
    QJsonObject envelope;
    envelope[QLatin1String(harkenplayableproperties::studio::kPresenceJsonKeyV1)] =
        encodePresence(item.playablePropertyPresence.bits);
    for (const Field& field : kAuthoredFields) {
        if (!isSlotApplicable(item.type, field.id)) continue;
        if ((item.playablePropertyPresence.bits & field.presentBit) == 0) continue;
        envelope[QLatin1String(field.jsonKey)] = valueToJson(
            componentFor(item, field)->getField(QLatin1String(field.storageField)),
            field);
    }
    if (!item.platformOverrides.rows.empty())
        envelope[QLatin1String(PlayablePropertyAuthoringV1::kPlatformOverridesJsonKey)] =
            platformOverridesToJson(item.platformOverrides, item.type == ProjectItem::Type::Bus);
    return envelope;
}

bool PlayablePropertyAuthoringV1::definitionFromJson(const QJsonObject& envelope,
                                                      ProjectItem& item,
                                                      QString* error)
{
    if (envelope.contains(QStringLiteral("spatializationMix"))
        || envelope.contains(QStringLiteral("spatial.spatializationMix"))) {
        setError(error, QStringLiteral("Spatialization Mix belongs to components.spatial"));
        return false;
    }
    const QLatin1String presenceKey(
        harkenplayableproperties::studio::kPresenceJsonKeyV1);
    if (!envelope.value(presenceKey).isString()) {
        setError(error, QStringLiteral("playableProperties.presence must be a string"));
        return false;
    }
    const auto presence = decodePresence(envelope.value(presenceKey).toString());
    if (!presence) {
        setError(error, QStringLiteral("playableProperties.presence is invalid"));
        return false;
    }

    struct PendingValue { IComponent* component; const Field* field; QVariant value; };
    std::vector<PendingValue> pending;
    pending.reserve(std::size(kAuthoredFields));
    for (const Field& field : kAuthoredFields) {
        if (isFieldGroupDefinitionType(item.type)
            && FieldGroupForProperty(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(field.id))) {
            if ((presence->bits & field.presentBit) != 0
                || envelope.contains(QLatin1String(field.jsonKey))) {
                setError(error, QStringLiteral("Grouped fields must be authored through fieldGroups"));
                return false;
            }
            continue;
        }
        const bool present = (presence->bits & field.presentBit) != 0;
        if (!isSlotApplicable(item.type, field.id)) {
            if (present) {
                setError(error, QStringLiteral(
                    "Playable property is present on a slot this type does not carry"));
                return false;
            }
            continue;
        }
        IComponent* component = componentFor(item, field);
        if (!component) {
            setError(error, QStringLiteral("Playable property storage component is missing"));
            return false;
        }
        QVariant value;
        if (present) {
            const QLatin1String key(field.jsonKey);
            if (!envelope.contains(key)) {
                setError(error, QStringLiteral("Present playable property has no value"));
                return false;
            }
            const auto parsed = valueFromJson(envelope.value(key), field);
            if (!parsed) {
                setError(error, QStringLiteral("Playable property value has invalid type"));
                return false;
            }
            value = *parsed;
        } else {
            value = canonicalValue(field);
        }
        pending.push_back({component, &field, value});
    }

    PlatformOverrideLedgerV1 overlays;
    if (!platformOverridesFromJson(
            envelope.value(QLatin1String(PlayablePropertyAuthoringV1::kPlatformOverridesJsonKey)),
            &overlays,
            error, item.type == ProjectItem::Type::Bus))
        return false;

    for (const PendingValue& value : pending) {
        if (!value.component->setField(QLatin1String(value.field->storageField),
                                       value.value)) {
            setError(error, QStringLiteral("Playable property storage rejected a value"));
            return false;
        }
    }
    item.playablePropertyPresence = *presence;
    item.platformOverrides = std::move(overlays);
    return true;
}

QJsonObject PlayablePropertyAuthoringV1::occurrenceToJson(
    const PlayablePropertyOccurrenceV1& occurrence, bool allowFieldGroupFields)
{
    if (!allowFieldGroupFields
        && (occurrence.presence.bits & kFieldGroupPropertyMask) != 0)
        return {};
    if ((occurrence.presence.bits & ~kAuthoredPresenceMask) != 0
        || !occurrence.hasExactWireTypes())
        return {};
    QJsonObject envelope;
    envelope[QLatin1String(harkenplayableproperties::studio::kPresenceJsonKeyV1)] =
        encodePresence(occurrence.presence.bits);
    for (const Field& field : kAuthoredFields) {
        if ((occurrence.presence.bits & field.presentBit) == 0) continue;
        const auto value = occurrenceValueToJson(
            occurrence.values[field.id - 1], field.wire);
        if (!value) return {};
        envelope[QLatin1String(field.jsonKey)] = *value;
    }
    return envelope;
}

std::optional<PlayablePropertyOccurrenceV1>
PlayablePropertyAuthoringV1::occurrenceFromJson(const QJsonObject& envelope,
                                                QString* error, bool allowFieldGroupFields)
{
    if (envelope.contains(QStringLiteral("spatializationMix"))
        || envelope.contains(QStringLiteral("spatial.spatializationMix"))) {
        setError(error, QStringLiteral("Occurrences cannot author Spatialization Mix"));
        return std::nullopt;
    }
    const QLatin1String presenceKey(
        harkenplayableproperties::studio::kPresenceJsonKeyV1);
    if (!envelope.value(presenceKey).isString()) {
        setError(error, QStringLiteral("Occurrence presence must be a string"));
        return std::nullopt;
    }
    const auto presence = decodePresence(envelope.value(presenceKey).toString());
    if (!presence) {
        setError(error, QStringLiteral("Occurrence presence is invalid"));
        return std::nullopt;
    }
    if (!allowFieldGroupFields) {
        for (const Field& field : kAuthoredFields) {
            if (FieldGroupForProperty(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(field.id))
                && ((presence->bits & field.presentBit) != 0
                    || envelope.contains(QLatin1String(field.jsonKey)))) {
                setError(error, QStringLiteral("Occurrences cannot author field group fields"));
                return std::nullopt;
            }
        }
    }
    PlayablePropertyOccurrenceV1 occurrence;
    occurrence.presence = *presence;
    for (const Field& field : kAuthoredFields) {
        if ((presence->bits & field.presentBit) == 0) continue;
        const QLatin1String key(field.jsonKey);
        if (!envelope.contains(key)) {
            setError(error, QStringLiteral("Present occurrence property has no value"));
            return std::nullopt;
        }
        const auto parsed = valueFromJson(envelope.value(key), field);
        if (!parsed) {
            setError(error, QStringLiteral("Occurrence property has invalid type"));
            return std::nullopt;
        }
        if (!occurrence.setValue(
                static_cast<harkenplayableproperties::PlayablePropertyIdV1>(field.id),
                occurrenceValue(*parsed, field))) {
            setError(error, QStringLiteral("Occurrence property wire is invalid"));
            return std::nullopt;
        }
    }
    return occurrence;
}

QJsonArray PlayablePropertyAuthoringV1::platformOverridesToJson(
    const PlatformOverrideLedgerV1& ledger, bool allowFieldGroupFields)
{
    QJsonArray rows;
    for (const auto& row : ledger.rows) {
        QJsonObject json = occurrenceToJson(row.properties, allowFieldGroupFields);
        json[QLatin1String(kPlatformJsonKey)] = ProjectPlatformSemantics::idString(row.platformId);
        rows.append(json);
    }
    return rows;
}

bool PlayablePropertyAuthoringV1::platformOverridesFromJson(
    const QJsonValue& value,
    PlatformOverrideLedgerV1* ledger,
    QString* error, bool allowFieldGroupFields)
{
    if (!ledger) return false;
    if (value.isUndefined() || value.isNull()) {
        ledger->rows.clear();
        return true;
    }
    if (!value.isArray()) {
        setError(error, QStringLiteral("platformOverrides must be an array"));
        return false;
    }
    PlatformOverrideLedgerV1 parsed;
    for (const QJsonValue& entry : value.toArray()) {
        if (!entry.isObject()) {
            setError(error, QStringLiteral("platformOverrides row must be an object"));
            return false;
        }
        const QJsonObject json = entry.toObject();
        if (json.contains(QStringLiteral("platform"))) {
            setError(error, QStringLiteral("Legacy platform overrides are unsupported; use platformId"));
            return false;
        }
        ProjectPlatformId platform = ProjectPlatformId{};
        if (!parseOverlayPlatform(json.value(QLatin1String(kPlatformJsonKey)),
                                  &platform,
                                  error))
            return false;
        if (parsed.find(platform)) {
            setError(error, QStringLiteral("platformOverrides platform is duplicated"));
            return false;
        }
        QJsonObject propertiesJson = json;
        propertiesJson.remove(QLatin1String(kPlatformJsonKey));
        auto properties = occurrenceFromJson(propertiesJson, error, allowFieldGroupFields);
        if (!properties) return false;
        parsed.ensure(platform)->properties = std::move(*properties);
    }
    *ledger = std::move(parsed);
    return true;
}

bool PlayablePropertyAuthoringV1::validateContainerOccurrenceTargets(
    const ProjectItem& container, const SelectionTable& ledger,
    QString* error)
{
    if (container.type != ProjectItemType::Container) {
        setError(error, QStringLiteral("Only a Container may own child occurrences"));
        return false;
    }
    if (!validateContainerOccurrenceLedgerShape(ledger, error)) return false;
    for (const auto& row : ledger.occurrenceRows()) {
        QString reason;
        if (!validateDirectPlaybackTarget(container, row.childGuid,
                findDirectPlaybackChild(container, row.childGuid), &reason)) {
            setError(error, QStringLiteral("Selection occurrence %1: %2")
                                .arg(row.stableOccurrenceOrdinal).arg(reason));
            return false;
        }
    }
    return true;
}

bool PlayablePropertyAuthoringV1::validateContainerOccurrences(
    const ProjectItem& container, QString* error)
{
    const auto* selection = container.comp<VSelection>();
    if (!selection) return container.type != ProjectItemType::Container;
    for (const auto* table : selection->selections.tables())
        if (!validateContainerOccurrenceTargets(container, *table, error)) return false;
    return true;
}

bool PlayablePropertyAuthoringV1::addContainerChildOccurrence(
    ProjectItem& container,
    const QUuid& childGuid,
    const std::optional<SelectionEntry>& restored,
    SelectionEntry* installed)
{
    auto* selection = container.comp<VSelection>();
    if (!selection) return false;
    auto next = selection->selections;
    if (!addContainerChildOccurrence(container, next.shared, childGuid, restored, installed))
        return false;
    if (!restored)
        for (auto& table : next.platforms)
            if (!addContainerChildOccurrence(container, table, childGuid)) return false;
    selection->selections = std::move(next);
    return true;
}

bool PlayablePropertyAuthoringV1::addContainerChildOccurrence(
    const ProjectItem& container, SelectionTable& ledger,
    const QUuid& childGuid,
    const std::optional<SelectionEntry>& restored,
    SelectionEntry* installed)
{
    if (container.type != ProjectItemType::Container || childGuid.isNull()) return false;
    SelectionTable candidate = ledger;
    SelectionEntry row;
    if (restored) {
        if (restored->childGuid != childGuid) return false;
        row = *restored;
        const uint64_t required = static_cast<uint64_t>(row.stableOccurrenceOrdinal) + 1;
        candidate.occurrenceOrdinalHighWater = std::max(
            candidate.occurrenceOrdinalHighWater, required);
    } else {
        auto& highWater = candidate.occurrenceOrdinalHighWater;
        if (highWater > std::numeric_limits<uint32_t>::max()) return false;
        row.childGuid = childGuid;
        row.stableOccurrenceOrdinal = static_cast<uint32_t>(highWater++);
    }
    auto rows = candidate.occurrenceRows();
    for (const auto& existing : rows)
        if (existing.stableOccurrenceOrdinal == row.stableOccurrenceOrdinal) return false;
    const auto insertAt = std::find_if(
        rows.begin(), rows.end(),
        [&](const auto& existing) {
            return existing.stableOccurrenceOrdinal > row.stableOccurrenceOrdinal;
        });
    rows.insert(insertAt, row);
    if (!candidate.setOccurrenceRows(rows)) return false;
    if (!validateContainerOccurrenceTargets(container, candidate)) return false;
    ledger = std::move(candidate);
    if (installed) *installed = row;
    return true;
}

std::optional<SelectionEntry>
PlayablePropertyAuthoringV1::removeContainerChildOccurrence(
    ProjectItem& container,
    const QUuid& childGuid)
{
    if (container.type != ProjectItemType::Container) return std::nullopt;
    auto& table = container.comp<VSelection>()->selections.shared;
    auto rows = table.occurrenceRows();
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
        return row.childGuid == childGuid;
    });
    if (it == rows.end()) return std::nullopt;
    SelectionEntry removed = *it;
    rows.erase(it);
    if (!table.setOccurrenceRows(rows)) return std::nullopt;
    return removed;
}

std::vector<SelectionEntry>
PlayablePropertyAuthoringV1::removeContainerChildOccurrences(
    ProjectItem& container, const QUuid& childGuid)
{
    std::vector<SelectionEntry> removed;
    auto* selection = container.comp<VSelection>();
    if (!selection) return removed;
    for (const auto& row : selection->selections.shared.occurrenceRows())
        if (row.childGuid == childGuid) removed.push_back(row);
    for (auto* table : selection->selections.tables())
        std::erase_if(table->entries, [&](const auto& member) { return member.childGuid == childGuid; });
    return removed;
}

std::optional<SelectionEntry>
PlayablePropertyAuthoringV1::removeContainerChildOccurrence(
    ProjectItem& container,
    uint32_t stableOccurrenceOrdinal)
{
    if (container.type != ProjectItemType::Container) return std::nullopt;
    auto& table = container.comp<VSelection>()->selections.shared;
    auto rows = table.occurrenceRows();
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
        return row.stableOccurrenceOrdinal == stableOccurrenceOrdinal;
    });
    if (it == rows.end()) return std::nullopt;
    SelectionEntry removed = *it;
    rows.erase(it);
    if (!table.setOccurrenceRows(rows)) return std::nullopt;
    return removed;
}

bool PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
    ProjectItem& container,
    uint32_t stableOccurrenceOrdinal,
    const QUuid& childGuid,
    SelectionEntry* previous)
{
    return replaceContainerChildOccurrence(container, container.comp<VSelection>()->selections.shared,
                                           stableOccurrenceOrdinal, childGuid, previous);
}

bool PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
    const ProjectItem& container, SelectionTable& ledger,
    uint32_t stableOccurrenceOrdinal, const QUuid& childGuid,
    SelectionEntry* previous)
{
    if (container.type != ProjectItemType::Container || childGuid.isNull()) return false;
    auto candidate = ledger;
    auto rows = candidate.occurrenceRows();
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& candidate) {
        return candidate.stableOccurrenceOrdinal == stableOccurrenceOrdinal;
    });
    if (row == rows.end()) return false;
    const auto oldRow = *row;
    row->childGuid = childGuid;
    if (!candidate.setOccurrenceRows(rows)) return false;
    if (!validateContainerOccurrenceTargets(container, candidate)) return false;
    ledger = std::move(candidate);
    if (previous) *previous = oldRow;
    return true;
}

}
