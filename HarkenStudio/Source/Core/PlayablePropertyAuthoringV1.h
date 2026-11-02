#pragma once
#include "Utils/ObjectId.h"

#include "harken_playable_property_studio.g.h"
#include "PlatformContext.h"
#include "SelectionData.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <cstdint>
#include <array>
#include <functional>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace VNX {

struct ProjectItem;
enum class ProjectItemType;
class PlayablePropertyAuthoringV1;

using PlayablePropertyOccurrenceValueV1 =
    std::variant<float, bool, uint32_t, harkenstudio::ObjectId, QJsonArray>;

struct PlayablePropertyOccurrenceV1
{
    harkenplayableproperties::ChassisPresenceV1 presence;

    PlayablePropertyOccurrenceV1();

    bool setValue(harkenplayableproperties::PlayablePropertyIdV1 id,
                  PlayablePropertyOccurrenceValueV1 value);
    const PlayablePropertyOccurrenceValueV1* value(
        harkenplayableproperties::PlayablePropertyIdV1 id) const noexcept;
    bool hasExactWireTypes() const noexcept;

    bool operator==(const PlayablePropertyOccurrenceV1& other) const
    {
        return presence.bits == other.presence.bits && values == other.values;
    }

private:
    std::array<
        PlayablePropertyOccurrenceValueV1,
        static_cast<std::size_t>(
            harkenplayableproperties::PlayablePropertyIdV1::SpatializationMix)> values;

    friend class PlayablePropertyAuthoringV1;
};

struct PlatformOverrideRowV1
{
    ProjectPlatformId platformId;
    PlayablePropertyOccurrenceV1 properties;
    QJsonObject filter;

    bool operator==(const PlatformOverrideRowV1&) const = default;
};

struct PlatformOverrideLedgerV1
{
    std::vector<PlatformOverrideRowV1> rows;

    bool operator==(const PlatformOverrideLedgerV1&) const = default;

    const PlatformOverrideRowV1* find(ProjectPlatformId platform) const
    {
        for (const auto& row : rows) {
            if (row.platformId == platform)
                return &row;
        }
        return nullptr;
    }

    PlatformOverrideRowV1* find(ProjectPlatformId platform)
    {
        return const_cast<PlatformOverrideRowV1*>(
            static_cast<const PlatformOverrideLedgerV1*>(this)->find(platform));
    }

    PlatformOverrideRowV1* ensure(ProjectPlatformId platform)
    {
        if (platform == ProjectPlatformId{})
            return nullptr;
        if (auto* existing = find(platform))
            return existing;
        auto it = std::lower_bound(
            rows.begin(),
            rows.end(),
            platform,
            [](const PlatformOverrideRowV1& row, ProjectPlatformId key) {
                return row.platformId < key;
            });
        return &*rows.insert(it, PlatformOverrideRowV1{platform, {}, {}});
    }

    bool erase(ProjectPlatformId platform)
    {
        auto it = std::find_if(rows.begin(), rows.end(),
                               [platform](const PlatformOverrideRowV1& row) {
                                   return row.platformId == platform;
                               });
        if (it == rows.end())
            return false;
        rows.erase(it);
        return true;
    }

    bool eraseEmpty()
    {
        const auto before = rows.size();
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [](const PlatformOverrideRowV1& row) {
                                      return row.properties.presence.bits == 0 && row.filter.isEmpty();
                                  }),
                   rows.end());
        return rows.size() != before;
    }

    bool eraseEmpty(ProjectPlatformId platform)
    {
        auto* row = find(platform);
        if (!row || row->properties.presence.bits != 0 || !row->filter.isEmpty())
            return false;
        return erase(platform);
    }

    bool keysUnique() const
    {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].platformId == ProjectPlatformId{})
                return false;
            for (size_t j = i + 1; j < rows.size(); ++j) {
                if (rows[i].platformId == rows[j].platformId)
                    return false;
            }
        }
        return true;
    }
};

enum class PlayablePropertyTargetKindV1 : uint8_t
{
    Definition = 0,
    TrackClip = 1,
    ContainerChild = 2,
};

struct PlayablePropertyLocatorV1
{
    PlayablePropertyTargetKindV1 kind = PlayablePropertyTargetKindV1::Definition;
    QUuid ownerGuid;
    QUuid occurrenceGuid;
    QUuid targetGuid;
    uint32_t stableOccurrenceOrdinal = 0;

    bool operator==(const PlayablePropertyLocatorV1&) const = default;
};

struct PlayablePropertyStateV1
{
    bool present = false;
    QJsonValue value;

    bool operator==(const PlayablePropertyStateV1&) const = default;
};

struct PlayablePropertySourceV1
{
    enum class Kind : uint8_t { Own = 0, Placement = 1, Inherited = 2 };

    PlayablePropertyLocatorV1 locator;
    ProjectItem* owner = nullptr;
    PlayablePropertyStateV1 state;
    Kind kind = Kind::Own;
};

enum class PlayablePropertyValueRuleV1 : uint8_t
{
    Local,
    Add,
    MergeByTarget,
    Inherit
};

class PlayablePropertyAuthoringV1
{
public:
    using Field = harkenplayableproperties::studio::JsonFieldV1;

    static std::span<const Field> fields() noexcept;
    static bool isComponentStored(uint8_t propertyId) noexcept;
    static std::optional<PlayablePropertySourceV1> resolveSpatializationMix(
        ProjectItem& subject,
        const std::function<ProjectItem*(const ProjectItem&)>& parentOf = {});
    static bool isDefinitionType(ProjectItemType type) noexcept;
    static bool isFieldGroupDefinitionType(ProjectItemType type) noexcept;
    static bool isSlotApplicable(ProjectItemType type, uint8_t propertyId) noexcept;
    static PlayablePropertyValueRuleV1 valueRule(
        ProjectItemType type, uint8_t propertyId) noexcept;
    // Authored slots the shipping editor deliberately does not expose: no row,
    // no command surface, no edit path. The slot, its storage, and its
    // serialization stay intact so existing projects keep whatever they
    // authored; only the editor surfaces are withdrawn.
    static bool isEditorExposed(uint8_t propertyId) noexcept;
    static bool isInspectorVisible(uint8_t propertyId) noexcept;
    static const Field* fieldById(uint8_t id) noexcept;
    static const Field* fieldByJsonKey(const QString& jsonKey) noexcept;
    static const Field* fieldByName(const QString& name) noexcept;
    static std::optional<QJsonValue> wireValueFromVariant(
        uint8_t propertyId, const QVariant& value);

    static std::optional<PlayablePropertyLocatorV1>
        definitionLocator(const ProjectItem& item);
    static std::optional<PlayablePropertyLocatorV1>
        trackClipLocator(const ProjectItem& track, const QUuid& clipGuid);
    static std::optional<PlayablePropertyLocatorV1>
        containerChildLocator(const ProjectItem& container, const QUuid& childGuid,
                              PlatformScope scope = std::nullopt);
    static std::optional<PlayablePropertyLocatorV1>
        containerChildLocator(const ProjectItem& container,
                              uint32_t stableOccurrenceOrdinal,
                              PlatformScope scope = std::nullopt);
    static uint32_t containerChildOccurrenceCount(const ProjectItem& container,
                                                  const QUuid& childGuid,
                                                  PlatformScope scope = std::nullopt);
    static std::optional<PlayablePropertyLocatorV1>
        placementLocator(const ProjectItem& subject,
                         const class PlacementContext& placement,
                         const std::function<ProjectItem*(const QUuid&)>& resolveItem);

    static std::optional<PlayablePropertyStateV1>
        readProperty(ProjectItem& owner,
                     const PlayablePropertyLocatorV1& locator,
                     uint8_t propertyId,
                     PlatformScope scope);
    static std::optional<PlayablePropertyStateV1>
        readPlatformOverlay(ProjectItem& owner,
                            const PlayablePropertyLocatorV1& locator,
                            uint8_t propertyId,
                            ProjectPlatformId platform);
    static bool restoreProperty(ProjectItem& owner,
                                const PlayablePropertyLocatorV1& locator,
                                uint8_t propertyId,
                                const PlayablePropertyStateV1& state,
                                PlatformScope scope);
    static bool setOverride(ProjectItem& owner,
                            const PlayablePropertyLocatorV1& locator,
                            uint8_t propertyId,
                            const QJsonValue& value,
                            PlatformScope scope);
    static bool clearOverride(ProjectItem& owner,
                              const PlayablePropertyLocatorV1& locator,
                              uint8_t propertyId,
                              PlatformScope scope);
    static std::optional<PlayablePropertySourceV1>
        resolveSource(ProjectItem& subject,
                      const PlayablePropertyLocatorV1& editedLocator,
                      uint8_t propertyId,
                      const class PlacementContext* placement,
                      const std::function<ProjectItem*(const QUuid&)>& resolveItem,
                      PlatformScope scope);

    static QString encodePresence(uint64_t bits);
    static std::optional<harkenplayableproperties::ChassisPresenceV1>
        decodePresence(const QString& text);

    static QJsonObject definitionToJson(const ProjectItem& item);
    static bool definitionFromJson(const QJsonObject& envelope,
                                   ProjectItem& item,
                                   QString* error = nullptr);

    static QJsonObject occurrenceToJson(
        const PlayablePropertyOccurrenceV1& occurrence,
        bool allowFieldGroupFields = false);
    static std::optional<PlayablePropertyOccurrenceV1> occurrenceFromJson(
        const QJsonObject& envelope,
        QString* error = nullptr,
        bool allowFieldGroupFields = false);

    static QJsonArray platformOverridesToJson(
        const PlatformOverrideLedgerV1& ledger,
        bool allowFieldGroupFields = false);
    static bool platformOverridesFromJson(const QJsonValue& value,
                                          PlatformOverrideLedgerV1* ledger,
                                          QString* error = nullptr,
                                          bool allowFieldGroupFields = false,
                                          bool allowFilter = false);

    static constexpr char kPlatformOverridesJsonKey[] = "platformOverrides";

    static bool validateContainerOccurrenceTargets(
        const ProjectItem& container, const SelectionTable& ledger,
        QString* error = nullptr);
    static bool validateContainerOccurrences(const ProjectItem& container,
                                             QString* error = nullptr);
    static bool addContainerChildOccurrence(
        ProjectItem& container,
        const QUuid& childGuid,
        const std::optional<SelectionEntry>& restored = std::nullopt,
        SelectionEntry* installed = nullptr);
    static bool addContainerChildOccurrence(
        const ProjectItem& container, SelectionTable& ledger,
        const QUuid& childGuid,
        const std::optional<SelectionEntry>& restored = std::nullopt,
        SelectionEntry* installed = nullptr);
    static std::optional<SelectionEntry>
        removeContainerChildOccurrence(ProjectItem& container,
                                       const QUuid& childGuid);
    static std::vector<SelectionEntry>
        removeContainerChildOccurrences(ProjectItem& container,
                                        const QUuid& childGuid);
    static std::optional<SelectionEntry>
        removeContainerChildOccurrence(ProjectItem& container,
                                       uint32_t stableOccurrenceOrdinal);
    static bool replaceContainerChildOccurrence(
        ProjectItem& container,
        uint32_t stableOccurrenceOrdinal,
        const QUuid& childGuid,
        SelectionEntry* previous = nullptr);
    static bool replaceContainerChildOccurrence(
        const ProjectItem& container, SelectionTable& ledger,
        uint32_t stableOccurrenceOrdinal, const QUuid& childGuid,
        SelectionEntry* previous = nullptr);
};

}
