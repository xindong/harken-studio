
#pragma once
#include "MixingSemantics.h"

#include <QString>
#include <QStringList>
#include <QList>
#include <QSet>
#include <QPointF>
#include <QUuid>
#include <QVariantMap>
#include <QJsonArray>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <functional>
#include <QJsonObject>
#include <QMap>
#include <QHash>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include "AssetPipeline/AssetTypes.h"
#include "ProjectPlatform.h"
#include "Component.h"
#include "Components/VSound.h"
#include "CurveSemantics.h"
#include "GridSemantics.h"
#include "VariableSemantics.h"
#include "ProjectFormatVersion.h"
#include "Timeline/TimelineTypes.h"
#include "TransitionSemantics.h"
#include "harken_playable_properties.g.h"
#include "PlayablePropertyAuthoringV1.h"
#include "FieldGroups.h"
#include "Utils/ObjectId.h"

namespace VNX {


struct MediaFileRef
{
    uint32_t sourceID = 0;
    QString relativePath;
    uint64_t lastModified = 0;
    uint32_t conversionHash = 0;
    QString language;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["sourceID"] = static_cast<qint64>(sourceID);
        obj["relativePath"] = relativePath;
        obj["lastModified"] = static_cast<qint64>(lastModified);
        obj["conversionHash"] = static_cast<qint64>(conversionHash);
        obj["language"] = language;
        return obj;
    }

    static MediaFileRef fromJson(const QJsonObject& obj) {
        MediaFileRef ref;
        ref.sourceID = static_cast<uint32_t>(obj["sourceID"].toInteger());
        ref.relativePath = obj["relativePath"].toString();
        ref.lastModified = static_cast<uint64_t>(obj["lastModified"].toInteger());
        ref.conversionHash = static_cast<uint32_t>(obj["conversionHash"].toInteger());
        ref.language = obj["language"].toString();
        return ref;
    }
};

enum class AudioChannelConfig {
    Mono,
    Stereo_2_0,
    Surround_5_1,
    Surround_7_1,
    Atmos_7_1_4
};

// Values are the v6 persisted-face contract (D1: dense repack, frozen for
// good). Declared ahead of ProjectItem so the view structs below can carry a
// typed item kind; `ProjectItem::Type` stays the spelling used everywhere.
enum class ProjectItemType {
    None = 0,
    Sound = 1,
    Container = 2,
    Track = 3,
    Piece = 4,
    Layer = 5,
    Bus = 6,
    Action = 7,
    Variable = 8,
    Attenuation = 9
};

struct Send
{
    uint32_t targetBusID = 0;
    float sendLevel = 0.0f;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["targetBusID"] = static_cast<qint64>(targetBusID);
        obj["sendLevel"] = sendLevel;
        return obj;
    }

    static Send fromJson(const QJsonObject& obj) {
        Send send;
        send.targetBusID = static_cast<uint32_t>(obj["targetBusID"].toInteger());
        send.sendLevel = static_cast<float>(obj["sendLevel"].toDouble());
        return send;
    }
};

// Journal of cross-references scrubbed when a subtree is deleted, so undo can
// replay them. Survivors reference into the deleted set through bus routing
// and sends.
struct DeletedSubtreeRefLog {
    struct FieldGroupRef {
        QUuid definition;
        DefinitionFieldGroups before;
        DefinitionFieldGroups after;
    };
    std::vector<FieldGroupRef> fieldGroups;
    struct ClipRef {
        uint32_t ownerObjectID = 0;
        int originalIndex = -1;
        Clip clip;
    };
    struct ActionTargetRef {
        uint32_t ownerObjectID = 0;
        size_t stepIndex = 0;
        uint32_t targetObjectID = 0;
        QString targetName;
    };
    struct FolderComponentRef {
        uint32_t ownerObjectID = 0;
        QJsonObject before;
        QJsonObject after;
    };
    struct SendRef {
        uint32_t ownerObjectID = 0;
        Send  send;
    };
    struct OutputBusRef {
        uint32_t itemObjectID = 0;
        uint32_t oldBusID = 0;
    };
    struct SelectionRef {
        uint32_t ownerObjectID = 0;
        SelectionData selections;
    };
    struct TransitionRef {
        uint32_t ownerObjectID = 0;
        TransitionTableDef table;
    };
    std::vector<SendRef>    sends;
    std::vector<OutputBusRef>  outputBusRefs;
    std::vector<ClipRef> clips;
    std::vector<ActionTargetRef> actionTargetRefs;
    std::vector<ActionTargetRef> actionTimingRefs;
    std::vector<FolderComponentRef> folderComponentRefs;
    QJsonObject folderProjectReferencesBefore;
    QJsonObject folderProjectReferencesAfter;
    std::vector<SelectionRef> selections;
    std::vector<TransitionRef> transitions;
    std::vector<SelectionEntry> containerChildOccurrences;
    bool empty() const
    {
        return fieldGroups.empty() && sends.empty() && outputBusRefs.empty()
            && actionTargetRefs.empty() && actionTimingRefs.empty() && folderComponentRefs.empty()
            && folderProjectReferencesBefore.isEmpty()
            && clips.empty() && selections.empty()
            && transitions.empty() && containerChildOccurrences.empty();
    }
};


struct SignalChainNode
{
    uint32_t objectID = 0;
    QString  name;
    QString  type;
    ProjectItemType itemType = ProjectItemType::None;
    bool     outputBusOverride = false;
    float    volume = 0.0f;
    float    pitch = 0.0f;
    int      childCount = 0;
    QString  mediaPath;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["objectID"] = static_cast<qint64>(objectID);
        obj["name"] = name;
        obj["type"] = type;
        obj["volume"] = volume;
        obj["pitch"] = pitch;
        obj["childCount"] = childCount;
        if (!mediaPath.isEmpty())
            obj["mediaPath"] = mediaPath;
        return obj;
    }
};

struct SendViewInfo
{
    uint32_t targetBusID = 0;
    QString  targetBusName;
    float    sendLevel = 0.0f;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["targetBusID"] = static_cast<qint64>(targetBusID);
        obj["targetBusName"] = targetBusName;
        obj["sendLevel"] = sendLevel;
        return obj;
    }
};

struct MainInputInfo
{
    uint32_t sourceID = 0;
    float levelDb = 0.0f;
};

struct IncomingSendInfo
{
    uint32_t fromBusID = 0;
    QString  fromName;
    float    level = 0.0f;
};

struct BusChainNode
{
    uint32_t busID = 0;
    QString  name;
    QString  type;
    float    volume = 0.0f;
    std::vector<SendViewInfo> sends;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["busID"] = static_cast<qint64>(busID);
        obj["name"] = name;
        obj["type"] = type;
        obj["volume"] = volume;
        QJsonArray sendArr;
        for (const auto& s : sends)
            sendArr.append(s.toJson());
        obj["sends"] = sendArr;
        return obj;
    }
};

struct SignalChainView
{
    SignalChainNode self;

    std::vector<SignalChainNode> children;

    std::vector<SignalChainNode> placementChain;

    std::vector<SignalChainNode> organizationChain;

    std::vector<BusChainNode> busChain;

    std::vector<IncomingSendInfo> incomingSends;

    std::vector<QString> breadcrumb;

    // Object that actually declares the main-output bus for `self`, resolved by
    // ReferenceSlotResolution; 0 when nothing along the chain declares one.
    uint32_t busSlotOwnerObjectID = 0;

    struct SlotSource
    {
        QString slot;
        uint32_t ownerObjectID = 0;
        QString ownerName;
        QString source;
        QString via;

        QJsonObject toJson() const {
            QJsonObject obj;
            obj["slot"] = slot;
            obj["ownerObjectID"] = static_cast<qint64>(ownerObjectID);
            obj["ownerName"] = ownerName;
            obj["source"] = source;
            obj["via"] = via;
            return obj;
        }
    };

    std::vector<SlotSource> slotSources;

    bool placementContextValid = true;
    QString placementContextError;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["self"] = self.toJson();

        QJsonArray childArr;
        for (const auto& c : children) childArr.append(c.toJson());
        obj["children"] = childArr;

        QJsonArray placementArr;
        for (const auto& p : placementChain) placementArr.append(p.toJson());
        obj["placementChain"] = placementArr;

        QJsonArray organizationArr;
        for (const auto& p : organizationChain) organizationArr.append(p.toJson());
        obj["organizationChain"] = organizationArr;

        QJsonArray busArr;
        for (const auto& b : busChain) busArr.append(b.toJson());
        obj["busChain"] = busArr;

        QJsonArray crumbs;
        for (const auto& s : breadcrumb) crumbs.append(s);
        obj["breadcrumb"] = crumbs;

        obj["busSlotOwnerObjectID"] = static_cast<qint64>(busSlotOwnerObjectID);

        QJsonArray sources;
        for (const auto& source : slotSources) sources.append(source.toJson());
        obj["slotSources"] = sources;

        obj["placementContextValid"] = placementContextValid;
        if (!placementContextError.isEmpty())
            obj["placementContextError"] = placementContextError;

        return obj;
    }
};

inline constexpr uint32_t kMasterBusID = 0;


constexpr int MAX_CONTAINER_DEPTH = 32;

struct ProjectConfig
{
    QString name;
    QString location;
    QVector<ProjectPlatform> platforms = ProjectPlatformSemantics::defaultPlatforms();
    int sampleRate = 48000;
    AudioChannelConfig channelConfig = AudioChannelConfig::Stereo_2_0;
    bool enableLFE = true;
    QString templateId;
    QJsonObject templateData;
    QString author;
    QString description;
};

struct AudioSourceData
{
    uint32_t sourceID = 0;
    QString  relativePath;
    QString  language;
    float    trimStartSec = 0.0f;
    float    trimEndSec = 0.0f;
    QString  conversionPreset;
};

enum class QuantizeTiming : uint8_t {
    Immediate,
    NextBeat,
    NextBar,
    NextMarker,
    NaturalEnd,
    CurrentLoopEnd,
    EntryMarker,
    ExitMarker
};

enum class QuantizeIfSilent : uint8_t { Immediate, Drop };
enum class ResetScope : uint8_t { Emitter, Global };

struct ActionQuantizeDef
{
    uint32_t referenceObjectID = 0;
    QString referenceName;
    QuantizeTiming timing = QuantizeTiming::Immediate;
    QuantizeIfSilent ifSilent = QuantizeIfSilent::Immediate;
};

struct VActionStep
{
    enum class Type : uint8_t {
        Play = 0,
        Stop = 1,
        PostAction = 2,
        SetVariable = 3,
        Reset = 4
    };

    Type type = Type::Play;
    uint32_t targetObjectID = 0;
    QString targetName;
    float delay = 0.0f;
    float fadeTime = 0.0f;
    ActionQuantizeDef quantize;
    float value = 0.0f;
    VariableScope scope = VariableScope::Emitter;
    ResetScope resetScope = ResetScope::Emitter;

    QString typeString() const
    {
        switch (type) {
            case Type::Play:          return "Play";
            case Type::Stop:          return "Stop";
            case Type::PostAction:     return "Post Action";
            case Type::SetVariable:   return "SetVariable";
            case Type::Reset:         return "Reset";
        }
        return {};
    }

    static std::optional<Type> typeFromString(const QString& s)
    {
        if (s == "Play")            return Type::Play;
        if (s == "Stop")            return Type::Stop;
        if (s == "PostAction")       return Type::PostAction;
        if (s == "Post Action")      return Type::PostAction;
        if (s == "SetVariable")      return Type::SetVariable;
        if (s == "Reset")            return Type::Reset;
        return std::nullopt;
    }
};

struct ParentRef
{
    enum class Kind : uint8_t {
        Root = 0,
        Bus,
        Item
    };

    Kind  kind = Kind::Root;
    QUuid guid;
    QUuid documentId;

    bool isRoot() const   { return kind == Kind::Root; }
    bool hasGuid() const  { return kind != Kind::Root; }

    QJsonObject toJson() const {
        QJsonObject obj;
        switch (kind) {
            case Kind::Root:   obj["kind"] = "root";   break;
            case Kind::Bus:    obj["kind"] = "bus";    break;
            case Kind::Item:   obj["kind"] = "item";   break;
        }
        if (hasGuid() && !guid.isNull())
            obj["id"] = guid.toString(QUuid::WithoutBraces);
        if (hasGuid() && !documentId.isNull())
            obj["documentId"] = documentId.toString(QUuid::WithoutBraces);
        return obj;
    }

    static std::optional<ParentRef> fromJson(const QJsonObject& obj) {
        ParentRef ref;
        const QString k = obj["kind"].toString();
        if      (k == "root")   ref.kind = Kind::Root;
        else if (k == "bus")    ref.kind = Kind::Bus;
        else if (k == "item")   ref.kind = Kind::Item;
        else                    return std::nullopt;
        if (!ref.hasGuid()) {
            if (obj.contains("id") || obj.contains("documentId"))
                return std::nullopt;
            return ref;
        }
        if (!obj["id"].isString()) return std::nullopt;
        ref.guid = QUuid::fromString(obj["id"].toString());
        if (ref.guid.isNull()) return std::nullopt;
        if (obj.contains("documentId")) {
            if (!obj["documentId"].isString()) return std::nullopt;
            ref.documentId = QUuid::fromString(obj["documentId"].toString());
            if (ref.documentId.isNull()) return std::nullopt;
        }
        return ref;
    }
};

struct ProjectItem
{
    using Type = ProjectItemType;

    QString name;
    Type type;
    QString sourcePath;
    uint32_t sourceID = 0;
    uint32_t objectID = 0;
    harkenstudio::ObjectId runtimeId() const { return runtimeObjectId(guid); }
    uint64_t nameBindingRevision = 1;

    QUuid    guid;
    QJsonObject documentHeader;
    QJsonObject extensions;

    QString  objectPath;

    QString  notes;

    uint32_t color = 0;

    bool expanded = false;
    harkenplayableproperties::ChassisPresenceV1 playablePropertyPresence;
    DefinitionFieldGroups fieldGroups;
    PlatformOverrideLedgerV1 platformOverrides;
    std::vector<std::shared_ptr<ProjectItem>> children;
    ProjectItem* parent = nullptr;

    ParentRef parentRef;

    bool dirtyBit = false;

    void markDirty()        { dirtyBit = true;  }
    void clearDirty()
    {
        dirtyBit = false;
        for (auto& child : children)
            if (child) child->clearDirty();
    }
    bool isDirtyObject() const { return dirtyBit; }

    bool outputBusOverride = false;
    bool isOutputBusOverride() const { return outputBusOverride; }
    void setOutputBusOverride(bool v) { outputBusOverride = v; }

    QPointF canvasPos = QPointF(0.0, 0.0);


    ComponentStore components;

    template<typename T> T* comp() { return components.get<T>(); }
    template<typename T> const T* comp() const { return components.get<T>(); }
    template<typename T> T* compOrCreate()
    {
        auto* component = components.getOrCreate<T>();
        if (component) component->setHostItem(this);
        return component;
    }

    ProjectItem() : type(Type::None) {}

    ProjectItem(const QString& n, Type t, const QString& source = QString(), uint32_t sid = 0)
        : name(n), type(t), sourcePath(source), sourceID(sid) {}
        
    virtual ~ProjectItem() = default;

    ProjectItem(ProjectItem&&) = default;
    ProjectItem& operator=(ProjectItem&&) = default;


    bool isSpatialObject() const {
        return false;
    }

    void attachDefaultComponents();
};


struct PhysicalFolder
{
    QString name;
    PhysicalFolder* parent = nullptr;
    std::vector<std::shared_ptr<PhysicalFolder>> children;
};

struct ProjectFileBinding
{
    std::shared_ptr<ProjectItem> rootItem;
    QString                       parentFolder;
};

struct ProjectDocumentBinding
{
    QUuid guid;
    QString filePath;
    QString parentFolder;
    PhysicalFolder* folder = nullptr;
};

struct ProjectFolderBinding
{
    QString parentFolder;
    std::shared_ptr<PhysicalFolder> rootFolder;
};

struct RootBindingSlot
{
    ProjectFileBinding binding;
    int                index = -1;
    bool valid() const { return binding.rootItem != nullptr; }
};

struct RootFolderSlot
{
    ProjectFolderBinding binding;
    int                  index = -1;
    bool valid() const { return binding.rootFolder != nullptr; }
};

struct PackGroup
{
    QString name;
    std::vector<harkenstudio::ObjectId> actionIds;
    bool allPlatforms = true;
    QStringList platformIds;

    bool includesPlatform(const QString& platform) const {
        return allPlatforms || platformIds.contains(platform);
    }

    bool operator==(const PackGroup& other) const {
        return name == other.name
            && actionIds == other.actionIds
            && allPlatforms == other.allPlatforms
            && platformIds == other.platformIds;
    }
    bool operator!=(const PackGroup& other) const { return !(*this == other); }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["name"] = name;
        QJsonArray idArray;
        for (harkenstudio::ObjectId id : actionIds) idArray.append(objectIdString(id));
        obj["actionIds"] = idArray;
        obj["allPlatforms"] = allPlatforms;
        QJsonArray platformArray;
        for (const auto& p : platformIds) platformArray.append(p);
        obj["platformIds"] = platformArray;
        return obj;
    }

    static PackGroup fromJson(const QJsonObject& obj) {
        PackGroup group;
        group.name = obj["name"].toString();
        for (const auto& v : obj["actionIds"].toArray()) {
            harkenstudio::ObjectId id{};
            if (!parseObjectIdJson(v, &id)) return {};
            group.actionIds.push_back(id);
        }
        QStringList rawPlatforms;
        for (const auto& v : obj["platformIds"].toArray())
            rawPlatforms.append(v.toString());
        group.platformIds = rawPlatforms;
        group.allPlatforms = obj["allPlatforms"].toBool();
        return group;
    }
};

struct ActionPublishedNameHistoryRow
{
    QString name;
    QUuid objectGuid;
    harkenstudio::ObjectId runtimeId() const { return runtimeObjectId(objectGuid); }
    uint64_t nameBindingRevision = 1;
    QString event;
    QString priorName;
};

enum class AttenuationDimension : uint8_t
{
    Volume = 0,
    Pitch  = 1,
};

inline const char* attenuationDimensionName(AttenuationDimension type)
{
    switch (type)
    {
    case AttenuationDimension::Pitch: return "pitch";
    default:                          return "volume";
    }
}

inline bool attenuationDimensionFromName(const QString& name, AttenuationDimension* out)
{
    const QString key = name.trimmed().toLower();
    if (key == "volume" || key == "volume_curve") { *out = AttenuationDimension::Volume; return true; }
    if (key == "pitch"  || key == "pitch_curve")  { *out = AttenuationDimension::Pitch;  return true; }
    return false;
}

inline void attenuationDimensionValueRange(AttenuationDimension type,
                                           float* yMin, float* yMax)
{
    *yMin = type == AttenuationDimension::Pitch ? -1200.0f : -96.0f;
    *yMax = type == AttenuationDimension::Pitch ?  1200.0f :   0.0f;
}

inline QVector<CurvePointDef> attenuationDefaultCurve(AttenuationDimension type,
                                                     float maxDistance)
{
    return {{0.0f, 0.0f},
            {maxDistance, type == AttenuationDimension::Pitch ? 0.0f : -96.0f}};
}

// Attenuation sugar over the unified curveNormalize: volume [-96, 0] mirrors
// the runtime's kAttenuationFloorDb rejection boundary, pitch stays
// editor-only.
inline void attenuationNormalizeCurve(AttenuationDimension type,
                                      QVector<CurvePointDef>& points,
                                      QVector<CurveHandlePairDef>& handles)
{
    float yMin = 0.0f;
    float yMax = 0.0f;
    attenuationDimensionValueRange(type, &yMin, &yMax);
    curveNormalize(yMin, yMax, points, handles);
}

struct AttenuationDimensionDef
{
    AttenuationDimension type = AttenuationDimension::Volume;
    QVector<CurvePointDef> curve;
    QVector<CurveHandlePairDef> handles;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["type"] = QLatin1String(attenuationDimensionName(type));
        QJsonArray points;
        for (const auto& p : curve)
            points.append(QJsonObject{{"distance", p.x}, {"value", p.value}});
        obj["points"] = points;
        // Same shape as the pack manifest: active segments only, omitted for
        // single-point curves.
        if (curve.size() > 1 && handles.size() == curve.size() - 1) {
            QJsonArray pairs;
            for (const auto& h : handles)
                pairs.append(QJsonArray{h.h1x, h.h1y, h.h2x, h.h2y});
            obj["handles"] = pairs;
        }
        return obj;
    }

    static bool fromJson(const QJsonObject& obj, AttenuationDimensionDef* out) {
        // Sole funnel for the one-cut migration: legacy "scaling"/"shape"
        // keys are dropped unread, values clamp into the dimension domain,
        // and absent or mispaired handles rebuild as straight lines — old
        // projects converge on load and solidify on the next save.
        AttenuationDimension type = AttenuationDimension::Volume;
        if (!attenuationDimensionFromName(obj["type"].toString(), &type))
            return false;
        AttenuationDimensionDef def;
        def.type = type;
        for (const auto& v : obj["points"].toArray()) {
            const QJsonObject po = v.toObject();
            def.curve.push_back({static_cast<float>(po["distance"].toDouble(0.0)),
                                 static_cast<float>(po["value"].toDouble(0.0))});
        }
        for (const auto& v : obj["handles"].toArray()) {
            const QJsonArray pair = v.toArray();
            if (pair.size() < 4) {
                def.handles.clear();
                break;
            }
            def.handles.push_back({static_cast<float>(pair.at(0).toDouble()),
                                   static_cast<float>(pair.at(1).toDouble()),
                                   static_cast<float>(pair.at(2).toDouble()),
                                   static_cast<float>(pair.at(3).toDouble())});
        }
        attenuationNormalizeCurve(def.type, def.curve, def.handles);
        *out = def;
        return true;
    }
};

struct AttenuationSnapshot
{
    QUuid   guid;
    QString name;
    float   maxDistance = 100.0f;
    QVector<AttenuationDimensionDef> dimensions;

    AttenuationDimensionDef* dimension(AttenuationDimension type) {
        for (auto& d : dimensions)
            if (d.type == type) return &d;
        return nullptr;
    }

    const AttenuationDimensionDef* dimension(AttenuationDimension type) const {
        for (const auto& d : dimensions)
            if (d.type == type) return &d;
        return nullptr;
    }

    AttenuationDimensionDef& ensureDimension(AttenuationDimension type) {
        if (auto* existing = dimension(type))
            return *existing;
        AttenuationDimensionDef def;
        def.type = type;
        dimensions.push_back(def);
        return dimensions.back();
    }

    bool removeDimension(AttenuationDimension type) {
        for (int i = 0; i < dimensions.size(); ++i) {
            if (dimensions[i].type == type) {
                dimensions.removeAt(i);
                return true;
            }
        }
        return false;
    }

    QJsonObject toJson() const {
        QJsonObject obj;
        if (!guid.isNull())
            obj["guid"] = guid.toString(QUuid::WithoutBraces);
        obj["name"]        = name;
        obj["maxDistance"] = maxDistance;
        QJsonArray dims;
        for (const auto& d : dimensions) dims.append(d.toJson());
        obj["dimensions"] = dims;
        return obj;
    }

    static AttenuationSnapshot fromJson(const QJsonObject& obj) {
        AttenuationSnapshot def;
        if (obj.contains("guid"))
            def.guid = QUuid::fromString(obj["guid"].toString());
        if (def.guid.isNull())
            def.guid = QUuid::createUuid();
        def.name        = obj["name"].toString();
        def.maxDistance = static_cast<float>(obj["maxDistance"].toDouble(100.0));
        for (const auto& v : obj["dimensions"].toArray()) {
            AttenuationDimensionDef dim;
            if (!AttenuationDimensionDef::fromJson(v.toObject(), &dim))
                continue;
            if (def.dimension(dim.type))
                continue;
            def.dimensions.push_back(dim);
        }
        return def;
    }
};


struct ProjectData
{
    QString projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject documentHeader;
    QJsonObject extensions;

    QString name;
    QString version = "1.0";
    QString author;
    QString description;
    QString createdDate;
    QString modifiedDate;
    
    QString projectPath;
    QString audioFolder;
    QString packFolder;

    QMap<uint32_t, QString> sourceAssets;
    
    std::vector<ProjectFileBinding> layers;
    std::vector<ProjectFileBinding> actions;
    std::vector<ProjectFileBinding> attenuationItems;
    std::vector<ProjectFileBinding> busses;
    std::vector<ProjectFileBinding> variables;
    std::vector<ProjectFolderBinding> folders;
    QHash<QUuid, ProjectDocumentBinding> documentBindings;

    std::vector<ProjectFileBinding*>       allFileBindings();
    std::vector<const ProjectFileBinding*> allFileBindings() const;
    std::vector<ProjectDocumentBinding*> allDocumentBindings();
    std::vector<const ProjectDocumentBinding*> allDocumentBindings() const;
    QString objectFilePath(const ProjectItem* item) const;
    PhysicalFolder* objectFolder(const ProjectItem* item) const;
    void setObjectFileLocation(const ProjectItem* item, PhysicalFolder* folder,
                               const QString& filePath);
    std::vector<ProjectDocumentBinding*> documentBindingsUnderFolder(const PhysicalFolder* folder);
    std::vector<const ProjectDocumentBinding*> documentBindingsUnderFolder(const PhysicalFolder* folder) const;
    QString itemPath(const ProjectItem* item) const;
    std::vector<ProjectItem*> findItemsBySelector(
        const QString& selector, std::optional<ProjectItem::Type> type = std::nullopt) const;
    ProjectItem* findItemBySelector(
        const QString& selector, std::optional<ProjectItem::Type> type = std::nullopt) const;
    std::vector<ProjectFileBinding>*       getBucketForType(ProjectItem::Type t);
    std::vector<ProjectFileBinding>*       getBucketForFolder(const QString& parentFolder);

    std::vector<PhysicalFolder*>           folderRootsForBucket(const QString& parentFolder);
    std::vector<const PhysicalFolder*>     folderRootsForBucket(const QString& parentFolder) const;
    PhysicalFolder*                        findFolderByPath(const QString& parentFolder,
                                                           const QString& relativePath);
    const PhysicalFolder*                  findFolderByPath(const QString& parentFolder,
                                                            const QString& relativePath) const;
    PhysicalFolder*                        ensureFolderPath(const QString& parentFolder,
                                                           const QString& relativePath);
    QString                                folderRelPath(const PhysicalFolder* folder) const;
    QString                                folderBucket(const PhysicalFolder* folder) const;
    const PhysicalFolder*                  folderForItem(const ProjectItem* item) const;
    QString                                itemFolderPath(const ProjectItem* item) const;
    std::vector<ProjectFileBinding*>       fileBindingsUnderFolder(const PhysicalFolder* folder);
    std::vector<const ProjectFileBinding*> fileBindingsUnderFolder(const PhysicalFolder* folder) const;

    QString                                folderNameForBucket(const std::vector<ProjectFileBinding>* bucket) const;





    QStringList projectLanguages;
    QString     defaultLanguage = "English";

    std::vector<PackGroup> packGroups;
    std::vector<ActionPublishedNameHistoryRow> actionPublishedNameHistory;

    QVector<ProjectPlatform> platforms;

    QStringList platformIds() const { return ProjectPlatformSemantics::ids(platforms); }
    int                sampleRate         = 48000;
    AudioChannelConfig channelConfig      = AudioChannelConfig::Stereo_2_0;
    bool               enableLFE          = true;
    QString            projectTemplateId  = "basic_3d";

    struct DeploymentProfile
    {
        QString profileId;
        QString name;
        QString engineKind;
        QString destination;
        QStringList platformIds;
        QStringList languages;
        bool deployPacks = true;
        bool deployManifest = true;
        bool deployCurrentWorld = false;
        bool autoDeployAfterBuild = false;
    };
    std::vector<DeploymentProfile> deploymentProfiles;

    QString packBuildOutputDir;
    bool buildExportWaveforms = false;
    QString buildWaveformOutputDir;

    std::vector<AudioSourceData> audioSources;

    QStringList actionNames;
    QStringList audioFiles;
    
    std::shared_ptr<ProjectItem> root;

    uint32_t nextObjectID = 1;

    uint32_t generateObjectID() {
        return nextObjectID++;
    }

};

struct ObjectIdRemap
{
    uint32_t oldObjectID = 0;
    uint32_t newObjectID = 0;
    QString  itemName;
};

struct ObjectIdRepairResult
{
    uint32_t nextObjectID = 1;
    std::vector<ObjectIdRemap> remaps;
};

inline void collectDirtyObjects(ProjectItem* root, std::vector<ProjectItem*>& out)
{
    if (!root) return;
    if (root->isDirtyObject()) {
        out.push_back(root);
    }
    for (auto& child : root->children) {
        collectDirtyObjects(child.get(), out);
    }
}

inline ProjectFileBinding* findFileBindingByRootGuid(ProjectData& project, const QString& guid)
{
    if (guid.isEmpty()) return nullptr;
    for (auto* binding : project.allFileBindings()) {
        if (binding && binding->rootItem
            && binding->rootItem->guid.toString(QUuid::WithoutBraces) == guid) {
            return binding;
        }
    }
    return nullptr;
}

inline ProjectItem* findPackRoot(ProjectItem* item)
{
    if (!item) return nullptr;
    while (item->parent) item = item->parent;
    return item;
}

inline ProjectFileBinding* findFileBindingForItem(ProjectData& project, ProjectItem* item)
{
    ProjectItem* root = findPackRoot(item);
    if (!root) return nullptr;
    return findFileBindingByRootGuid(project, root->guid.toString(QUuid::WithoutBraces));
}

std::vector<MediaFileRef> collectMediaFiles(const ProjectItem* root);
std::vector<uint32_t> getOwnedMedia(const ProjectItem* root);
bool ownsMedia(const ProjectItem* root, uint32_t sourceID);
const MediaFileRef* getMediaRef(const ProjectItem* root, uint32_t sourceID);

inline bool validateProjectTree(const ProjectItem* root)
{
    if (!root) return false;

    std::function<bool(const std::shared_ptr<ProjectItem>&, const ProjectItem*)> checkParents =
        [&](const std::shared_ptr<ProjectItem>& item, const ProjectItem* expectedParent) -> bool {
        if (!item) return false;
        if (item->parent != expectedParent) return false;
        for (const auto& child : item->children) {
            if (!checkParents(child, item.get())) return false;
        }
        return true;
    };

    for (const auto& child : root->children) {
        if (!checkParents(child, root)) return false;
    }
    return true;
}

bool validateProjectObjectIdentities(const ProjectData& project, QString* error = nullptr);

inline ObjectIdRepairResult ensureUniqueObjectIds(ProjectData& project)
{
    ObjectIdRepairResult result;

    const auto forEachRootInLoadOrder = [&project](auto&& fn) {
        if (project.root) fn(project.root.get());
        std::vector<ProjectFileBinding>* buckets[] = {
            &project.busses, &project.actions,
            &project.attenuationItems, &project.variables,
            &project.layers };
        for (auto* bucket : buckets)
            for (auto& binding : *bucket) {
                if (!binding.rootItem) continue;
                if (binding.rootItem->parentRef.hasGuid())
                    continue;
                fn(binding.rootItem.get());
            }
    };

    uint32_t maxExisting = 0;
    auto visitMax = [&maxExisting](ProjectItem* item, auto&& visitRef) -> void {
        if (!item) return;
        if (item->objectID > maxExisting) maxExisting = item->objectID;
        for (auto& child : item->children) visitRef(child.get(), visitRef);
    };
    forEachRootInLoadOrder([&](ProjectItem* root) { visitMax(root, visitMax); });

    if (project.nextObjectID <= maxExisting)
        project.nextObjectID = maxExisting + 1;
    if (project.nextObjectID == 0)
        project.nextObjectID = 1;

    std::map<uint32_t, const ProjectItem*> used;
    auto repair = [&](ProjectItem* item, auto&& repairRef) -> void {
        if (!item) return;
        if (item->objectID != 0) {
            const auto ins = used.emplace(item->objectID, item);
            if (!ins.second && ins.first->second != item) {
                ObjectIdRemap remap;
                remap.oldObjectID = item->objectID;
                remap.itemName = item->name;
                item->objectID = project.generateObjectID();
                used.emplace(item->objectID, item);
                remap.newObjectID = item->objectID;
                result.remaps.push_back(std::move(remap));
            }
        }
        for (auto& child : item->children) repairRef(child.get(), repairRef);
    };
    forEachRootInLoadOrder([&](ProjectItem* root) { repair(root, repair); });

    result.nextObjectID = project.nextObjectID;
    return result;
}

}
