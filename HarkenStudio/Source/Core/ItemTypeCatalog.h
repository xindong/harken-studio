
#pragma once

#include "ProjectTypes.h"

#include <QString>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace VNX {
namespace ItemTypeCatalog {

enum class Bucket : uint8_t {
    None = 0,
    Layers,
    Busses,
    Actions,
    Variables,
    Attenuations
};

enum class NameNamespace : uint8_t {
    None = 0,
    AudioNode,
    Action,
    Attenuation,
    Bus,
    Variable
};

enum class CreateParentPolicy { DefaultForType, SelectedItem, SelectedGuarded };

struct IconSpec {
    const char* logicalId;
};

struct TypeFacts {
    ProjectItem::Type type;
    const char* canonicalName;
    const char* displayName;
    IconSpec icon;
    Bucket bucket;
    bool topLevelCreatable;
    NameNamespace nameNamespace;
    const ProjectItem::Type* allowedParents;
    size_t allowedParentCount;
    bool acceptsImportedAudio;
    bool contentObject;
    bool mixingCapability;
    bool spatialCapability;
    bool triggerable;
    bool packDependencyNode;
    bool blueprintChainNode;
    bool folderAddressable;
};

struct BucketFacts {
    Bucket bucket;
    const char* folderName;
    const char* displayName;
    bool allowsFolder;
    bool acceptsAudioImport;
};

struct CreationSpec {
    ProjectItem::Type type;
    const char* defaultName;
    CreateParentPolicy parentPolicy;
    ProjectItem::Type requiredParent;
    bool selectAfter;
    bool separatorAfter;
    bool comingSoon;
};

inline constexpr char kLayersFolderName[]       = "Layers";
inline constexpr char kBussesFolderName[]       = "Busses";
inline constexpr char kActionsFolderName[]      = "Actions";
inline constexpr char kVariablesFolderName[]    = "Variables";
inline constexpr char kAttenuationsFolderName[] = "Attenuations";

const TypeFacts* factsOrNull(ProjectItem::Type type);
const std::array<ProjectItem::Type, 9>& allTypes();

QString displayName(ProjectItem::Type type);
QString canonicalName(ProjectItem::Type type);
QString ipcName(ProjectItem::Type type);
QString blueprintChainName(ProjectItem::Type type);
QString packDependencyName(ProjectItem::Type type);

std::optional<ProjectItem::Type> parseTypeName(const QString& name,
                                               Qt::CaseSensitivity cs,
                                               bool acceptAliases);

bool canBeChildOf(ProjectItem::Type child, ProjectItem::Type parent);
bool canAcceptAnyChild(ProjectItem::Type parent);
std::vector<ProjectItem::Type> allowedChildTypesOf(ProjectItem::Type parent);
std::vector<ProjectItem::Type> userFacingChildTypesOf(ProjectItem::Type parent);
bool isUserFacingType(ProjectItem::Type type);
bool acceptsImportedAudio(ProjectItem::Type type);

bool isContentObject(ProjectItem::Type type);
bool isTimelineClipTarget(ProjectItem::Type type);
bool hasMixingCapability(ProjectItem::Type type);
bool hasSpatialCapability(ProjectItem::Type type);
bool isTriggerable(ProjectItem::Type type);
bool isFolderAddressable(ProjectItem::Type type);
bool allowsLoop(ProjectItem::Type type);

const BucketFacts* bucketFacts(Bucket bucket);
const BucketFacts* bucketFactsForFolder(const QString& folderName);
Bucket bucketOf(ProjectItem::Type type);
QString folderNameOf(Bucket bucket);
const std::array<Bucket, 5>& allBuckets();
QString bucketDisplayName(const QString& folderName);
std::vector<ProjectItem::Type> allowedTopLevelTypesForBucket(const QString& folderName);
bool bucketAllowsFolder(const QString& folderName);
bool bucketAcceptsAudioImport(const QString& folderName);

const std::vector<CreationSpec>& creationSpecs();
NameNamespace nameNamespaceOf(ProjectItem::Type type);
QString nameNamespaceDisplayName(NameNamespace ns);
IconSpec iconSpecOf(ProjectItem::Type type);

}
}
