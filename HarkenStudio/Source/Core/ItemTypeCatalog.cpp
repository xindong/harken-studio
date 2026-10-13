
#include "ItemTypeCatalog.h"

namespace VNX {
namespace ItemTypeCatalog {

namespace {

using T = ProjectItem::Type;

constexpr T kSoundParents[]       = { T::Layer, T::Container, T::Track };
constexpr T kContainerParents[]   = { T::Layer, T::Container, T::Track };
constexpr T kTrackParents[]       = { T::Layer, T::Container, T::Piece };
constexpr T kPieceParents[]       = { T::Layer, T::Container, T::Track };
constexpr T kLayerParents[]       = { T::Layer };
constexpr T kBusParents[]         = { T::Bus };

// Row order = allTypes() order; allowedChildTypesOf / allowedTopLevelTypesForBucket
// output order derives from it. Trailing bool columns after allowedParentCount:
// acceptsImportedAudio, contentObject, mixingCapability, spatialCapability,
// triggerable, packDependencyNode, blueprintChainNode, folderAddressable.
constexpr TypeFacts kTypeFacts[] = {
    { T::Sound,       "Sound",       "Sound",
      { "projectItem.sound" },
      Bucket::Layers,       true,  NameNamespace::AudioNode,
      kSoundParents, 3,
      true,  true,  true,  true,  true,  true,  true,  true  },

    { T::Container,   "Container",   "Container",
      { "projectItem.container" },
      Bucket::Layers,       true,  NameNamespace::AudioNode,
      kContainerParents, 3,
      true,  true,  true,  true,  true,  true,  true,  true  },

    { T::Track,       "Track",       "Track",
      { "projectItem.track" },
      Bucket::Layers,       true,  NameNamespace::AudioNode,
      kTrackParents, 3,
      true,  true,  true,  true,  true,  true,  true,  true  },

    { T::Piece,       "Piece",       "Piece",
      { "projectItem.piece" },
      Bucket::Layers,       true,  NameNamespace::AudioNode,
      kPieceParents, 3,
      false, true,  true,  true,  true,  true,  true,  true  },

    { T::Layer,       "Layer",       "Layer",
      { "projectItem.layer" },
      Bucket::Layers,       true,  NameNamespace::AudioNode,
      kLayerParents, 1,
      true,  false, true,  true,  false, true,  true,  true  },

    { T::Bus,         "Bus",         "Bus",
      { "projectItem.bus" },
      Bucket::Busses,       false, NameNamespace::Bus,
      kBusParents, 1,
      false, false, true,  false, false, false, true,  false },

    { T::Action,      "Action",      "Action",
      { "projectItem.action" },
      Bucket::Actions,      true,  NameNamespace::Action,
      nullptr, 0,
      false, false, false, false, true,  true,  true,  true  },

    { T::Variable,    "Variable",    "Variable",
      { "projectItem.variable" },
      Bucket::Variables,    true,  NameNamespace::Variable,
      nullptr, 0,
      false, false, false, false, false, false, false, true  },

    { T::Attenuation, "Attenuation", "Attenuation",
      { "projectItem.attenuation" },
      Bucket::Attenuations, true,  NameNamespace::Attenuation,
      nullptr, 0,
      false, false, false, false, false, false, true,  true  },
};

struct AliasEntry { const char* name; T type; };

constexpr std::array<AliasEntry, 0> kAliases{};

// Order = ProjectManager::getStandardDirectories() serialization order.
constexpr BucketFacts kBucketFacts[] = {
    { Bucket::Layers,       kLayersFolderName,       "Layer",       true,  true  },
    { Bucket::Busses,       kBussesFolderName,       "Bus",         false, false },
    { Bucket::Actions,      kActionsFolderName,      "Action",      true,  false },
    { Bucket::Variables,    kVariablesFolderName,    "Variable",    true,  false },
    { Bucket::Attenuations, kAttenuationsFolderName, "Attenuation", true,  false },
};

constexpr char kUnknownName[] = "Unknown";

}

const TypeFacts* factsOrNull(ProjectItem::Type type)
{
    for (const TypeFacts& facts : kTypeFacts) {
        if (facts.type == type) return &facts;
    }
    return nullptr;
}

const std::array<ProjectItem::Type, 9>& allTypes()
{
    static const std::array<ProjectItem::Type, 9> types = {
        T::Sound, T::Container, T::Track, T::Piece,
        T::Layer, T::Bus, T::Action, T::Variable, T::Attenuation,
    };
    return types;
}

QString displayName(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    return QString::fromLatin1(facts ? facts->displayName : kUnknownName);
}

QString canonicalName(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    return QString::fromLatin1(facts ? facts->canonicalName : kUnknownName);
}

QString ipcName(ProjectItem::Type type)
{
    return canonicalName(type);
}

QString blueprintChainName(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    if (!facts || !facts->blueprintChainNode) return QString::fromLatin1(kUnknownName);
    return QString::fromLatin1(facts->canonicalName);
}

QString packDependencyName(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    if (!facts || !facts->packDependencyNode) return QString();
    return QString::fromLatin1(facts->canonicalName);
}

std::optional<ProjectItem::Type> parseTypeName(const QString& name,
                                               Qt::CaseSensitivity cs,
                                               bool acceptAliases)
{
    for (const TypeFacts& facts : kTypeFacts) {
        if (name.compare(QLatin1String(facts.canonicalName), cs) == 0)
            return facts.type;
    }
    if (acceptAliases) {
        for (const AliasEntry& alias : kAliases) {
            if (name.compare(QLatin1String(alias.name), cs) == 0)
                return alias.type;
        }
    }
    return std::nullopt;
}

bool canBeChildOf(ProjectItem::Type child, ProjectItem::Type parent)
{
    const TypeFacts* facts = factsOrNull(child);
    if (!facts) return false;
    for (size_t i = 0; i < facts->allowedParentCount; ++i) {
        if (facts->allowedParents[i] == parent) return true;
    }
    return false;
}

bool canAcceptAnyChild(ProjectItem::Type parent)
{
    for (ProjectItem::Type child : allTypes()) {
        if (canBeChildOf(child, parent)) return true;
    }
    return false;
}

std::vector<ProjectItem::Type> allowedChildTypesOf(ProjectItem::Type parent)
{
    std::vector<ProjectItem::Type> out;
    for (ProjectItem::Type child : allTypes()) {
        if (canBeChildOf(child, parent)) out.push_back(child);
    }
    return out;
}

bool isUserFacingType(ProjectItem::Type type)
{
    return factsOrNull(type) != nullptr;
}

std::vector<ProjectItem::Type> userFacingChildTypesOf(ProjectItem::Type parent)
{
    std::vector<ProjectItem::Type> out;
    for (ProjectItem::Type child : allowedChildTypesOf(parent)) {
        if (isUserFacingType(child)) out.push_back(child);
    }
    return out;
}

bool acceptsImportedAudio(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    return facts && facts->acceptsImportedAudio;
}

bool isContentObject(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    return facts && facts->contentObject;
}

bool isTimelineClipTarget(ProjectItem::Type type)
{
    return type == T::Sound || type == T::Container || type == T::Piece;
}

bool hasMixingCapability(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    return facts && facts->mixingCapability;
}

bool hasSpatialCapability(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    return facts && facts->spatialCapability;
}

bool isTriggerable(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    return facts && facts->triggerable;
}

bool isFolderAddressable(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    return facts && facts->folderAddressable;
}

bool allowsLoop(ProjectItem::Type type)
{
    return isContentObject(type);
}

const BucketFacts* bucketFacts(Bucket bucket)
{
    for (const BucketFacts& facts : kBucketFacts) {
        if (facts.bucket == bucket) return &facts;
    }
    return nullptr;
}

const BucketFacts* bucketFactsForFolder(const QString& folderName)
{
    for (const BucketFacts& facts : kBucketFacts) {
        if (folderName == QLatin1String(facts.folderName)) return &facts;
    }
    return nullptr;
}

Bucket bucketOf(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    return facts ? facts->bucket : Bucket::None;
}

QString folderNameOf(Bucket bucket)
{
    const BucketFacts* facts = bucketFacts(bucket);
    return facts ? QString::fromLatin1(facts->folderName) : QString();
}

const std::array<Bucket, 5>& allBuckets()
{
    static const std::array<Bucket, 5> buckets = {
        Bucket::Layers, Bucket::Busses, Bucket::Actions,
        Bucket::Variables, Bucket::Attenuations,
    };
    return buckets;
}

QString bucketDisplayName(const QString& folderName)
{
    const BucketFacts* facts = bucketFactsForFolder(folderName);
    return facts ? QString::fromLatin1(facts->displayName) : folderName;
}

std::vector<ProjectItem::Type> allowedTopLevelTypesForBucket(const QString& folderName)
{
    const BucketFacts* bucket = bucketFactsForFolder(folderName);
    if (!bucket) return {};
    std::vector<ProjectItem::Type> out;
    for (ProjectItem::Type type : allTypes()) {
        const TypeFacts* facts = factsOrNull(type);
        if (facts->topLevelCreatable && facts->bucket == bucket->bucket)
            out.push_back(type);
    }
    return out;
}

bool bucketAllowsFolder(const QString& folderName)
{
    const BucketFacts* facts = bucketFactsForFolder(folderName);
    return facts && facts->allowsFolder;
}

bool bucketAcceptsAudioImport(const QString& folderName)
{
    const BucketFacts* facts = bucketFactsForFolder(folderName);
    return facts && facts->acceptsAudioImport;
}

const std::vector<CreationSpec>& creationSpecs()
{
    static const std::vector<CreationSpec> specs = {
        { T::Action,      "Play_",
          CreateParentPolicy::DefaultForType, T::None, true,  false, false },
        { T::Sound,       "New_Sound",
          CreateParentPolicy::DefaultForType, T::None, true,  false, false },
        { T::Container,   "New_Container",
          CreateParentPolicy::DefaultForType, T::None, true,  false, false },
        { T::Track,       "New_Track",
          CreateParentPolicy::DefaultForType, T::None, true,  false, false },
        { T::Piece,       "New_Piece",
          CreateParentPolicy::DefaultForType, T::None, true,  false, false },
        { T::Layer,       "New_Layer",
          CreateParentPolicy::DefaultForType, T::None, true,  false, false },
        { T::Attenuation, "Default_Attenuation",
          CreateParentPolicy::DefaultForType, T::None, true,  false, false },
        { T::Variable,    "New_Variable",
          CreateParentPolicy::DefaultForType, T::None, true,  false, false },
        { T::Bus,         "New_Bus",
          CreateParentPolicy::SelectedItem,   T::Bus,    true,  false, false },
    };
    return specs;
}

NameNamespace nameNamespaceOf(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    return facts ? facts->nameNamespace : NameNamespace::None;
}

QString nameNamespaceDisplayName(NameNamespace ns)
{
    switch (ns) {
        case NameNamespace::None:        return {};
        case NameNamespace::AudioNode:   return QStringLiteral("Sounds, Containers and Layers");
        case NameNamespace::Action:      return QStringLiteral("Actions");
        case NameNamespace::Attenuation: return QStringLiteral("Attenuations");
        case NameNamespace::Bus:         return QStringLiteral("Buses");
        case NameNamespace::Variable:    return QStringLiteral("Variables");
    }
    return {};
}

IconSpec iconSpecOf(ProjectItem::Type type)
{
    const TypeFacts* facts = factsOrNull(type);
    return facts ? facts->icon : IconSpec{ "projectItem.folder" };
}

}

// ProjectData bucket accessors are defined here (not in a ProjectTypes.cpp)
// so ProjectTypes.h never includes ItemTypeCatalog.h. The switch below is the
// single retained Bucket -> member-vector structural binding; allFileBindings
// iterates allBuckets() through it, so adding a bucket means one case here.
namespace {

const std::vector<ProjectFileBinding>* bucketMember(const ProjectData& data,
                                                    ItemTypeCatalog::Bucket bucket)
{
    switch (bucket) {
        case ItemTypeCatalog::Bucket::Layers:       return &data.layers;
        case ItemTypeCatalog::Bucket::Actions:      return &data.actions;
        case ItemTypeCatalog::Bucket::Attenuations: return &data.attenuationItems;
        case ItemTypeCatalog::Bucket::Busses:       return &data.busses;
        case ItemTypeCatalog::Bucket::Variables:    return &data.variables;
        default:                                    return nullptr;
    }
}

}

std::vector<ProjectFileBinding>* ProjectData::getBucketForType(ProjectItem::Type t)
{
    return const_cast<std::vector<ProjectFileBinding>*>(
        bucketMember(*this, ItemTypeCatalog::bucketOf(t)));
}

std::vector<ProjectFileBinding>* ProjectData::getBucketForFolder(const QString& parentFolder)
{
    const ItemTypeCatalog::BucketFacts* facts =
        ItemTypeCatalog::bucketFactsForFolder(parentFolder);
    if (!facts) return nullptr;
    return const_cast<std::vector<ProjectFileBinding>*>(bucketMember(*this, facts->bucket));
}

QString ProjectData::folderNameForBucket(const std::vector<ProjectFileBinding>* bucket) const
{
    for (ItemTypeCatalog::Bucket b : ItemTypeCatalog::allBuckets()) {
        if (bucketMember(*this, b) == bucket)
            return ItemTypeCatalog::folderNameOf(b);
    }
    return QString();
}

std::vector<const ProjectFileBinding*> ProjectData::allFileBindings() const
{
    std::vector<const ProjectFileBinding*> out;
    for (ItemTypeCatalog::Bucket b : ItemTypeCatalog::allBuckets()) {
        const std::vector<ProjectFileBinding>* bucket = bucketMember(*this, b);
        if (!bucket) continue;
        out.reserve(out.size() + bucket->size());
        for (const ProjectFileBinding& binding : *bucket) {
            out.push_back(&binding);
        }
    }
    return out;
}

std::vector<ProjectFileBinding*> ProjectData::allFileBindings()
{
    std::vector<ProjectFileBinding*> out;
    for (const ProjectFileBinding* binding :
         static_cast<const ProjectData&>(*this).allFileBindings())
        out.push_back(const_cast<ProjectFileBinding*>(binding));
    return out;
}

}
