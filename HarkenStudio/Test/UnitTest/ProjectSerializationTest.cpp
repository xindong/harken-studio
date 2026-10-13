#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/FieldGroupTestUtils.h"
#include "ItemTypeCatalog.h"
#include "NameValidator.h"
#include "ProjectManager.h"
#include "ProjectSerializer.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "Components/VAction.h"
#include "Components/VAttenuation.h"
#include "Components/VAudio.h"
#include "Components/VBus.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "TransitionSemantics.h"
#include "Support/ProjectDocumentTestUtils.h"
#include "Support/RuntimeIdentityTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>

#include <functional>
#include <initializer_list>
#include <set>
#include <unordered_map>
#include "Support/ProjectPlatformTestUtils.h"

using VNX::ParentRef;
using VNX::BindingLedger;
using VNX::ProjectData;
using VNX::ProjectFileBinding;
using VNX::ProjectItem;
using VNX::VariableKind;
using VNX::VariableLabel;
using VNX::VariableScope;
using VNX::VAudio;
using VNX::VMixNode;
using VNX::VSpatial;
using VNX::VVariable;

namespace {

QString objectFilePath(const ProjectItem* item)
{
    QString path = VNX::ItemTypeCatalog::folderNameOf(VNX::ItemTypeCatalog::bucketOf(item->type));
    const QString folder = VNX::ProjectManager::instance().currentProject()->itemFolderPath(item);
    if (!folder.isEmpty()) path += QLatin1Char('/') + folder;
    return path + QLatin1Char('/') + VNX::NameValidator::objectFileName(item->name, item->guid);
}

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QString projectFilesText(const QString& root)
{
    QString all;
    QDirIterator it(root,
                    QStringList() << QStringLiteral("*.hsp") << QStringLiteral("*.vp"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile f(it.next());
        if (f.open(QIODevice::ReadOnly))
            all += QString::fromUtf8(f.readAll());
    }
    return all;
}

std::shared_ptr<ProjectItem> addChild(ProjectItem& parent,
                                      const char* name,
                                      ProjectItem::Type type)
{
    auto child = std::make_shared<ProjectItem>(QLatin1String(name), type);
    child->attachDefaultComponents();
    child->guid = QUuid::createUuid();
    child->parent = &parent;
    parent.children.push_back(child);
    if (parent.type == ProjectItem::Type::Container) {
        EXPECT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(
            parent, child->guid));
    }
    return child;
}

QMap<QString, QByteArray> projectFileBytes(const QString& root)
{
    QMap<QString, QByteArray> out;
    const QDir rootDir(root);
    QDirIterator it(root,
                    QStringList() << QStringLiteral("*.hsp") << QStringLiteral("*.vp"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString abs = it.next();
        QFile f(abs);
        if (f.open(QIODevice::ReadOnly))
            out.insert(rootDir.relativeFilePath(abs), f.readAll());
    }
    return out;
}

void markPlayablePropertiesPresent(
    ProjectItem& item,
    std::initializer_list<harkenplayableproperties::PlayablePropertyIdV1> fields)
{
    for (const auto field : fields)
        item.playablePropertyPresence.Set(field);
}

QJsonObject canonicalPlayableProperties()
{
    return QJsonObject{{QStringLiteral("presence"),
                        QStringLiteral("0000000000000000")}};
}

class SpatialReferenceSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ProjectItem sound(QStringLiteral("Shot"), ProjectItem::Type::Sound);
        sound.guid = QUuid::createUuid();
        sound.guid = VNX::authoringUuid(harkentest::StudioObjectId(101));
        sound.attachDefaultComponents();
        VNX::ProjectSerializer serializer;
        sound_ = serializer.projectItemToJson(sound);
        attenuation_ = {
            {QStringLiteral("guid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("name"), QStringLiteral("Cliff")},
            {QStringLiteral("type"), QStringLiteral("Attenuation")},
            {QStringLiteral("runtimeStableId"), 4107},
        };
        other_ = attenuation_;
        other_[QStringLiteral("guid")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        other_[QStringLiteral("name")] = QStringLiteral("Valley");
        other_[QStringLiteral("runtimeStableId")] = 9109;
    }

    void setLegacyReference(const QJsonObject& legacy)
    {
        QJsonObject components = sound_.value(QStringLiteral("components")).toObject();
        QJsonObject spatial = components.value(QStringLiteral("spatial")).toObject();
        for (auto field = legacy.begin(); field != legacy.end(); ++field)
            spatial[field.key()] = field.value();
        components[QStringLiteral("spatial")] = spatial;
        sound_[QStringLiteral("components")] = components;
    }

    QJsonObject canonicalReference(const QJsonObject& target) const
    {
        return {{QStringLiteral("id"), target.value(QStringLiteral("guid"))}};
    }

    void setCanonicalReference(const QJsonValue& reference)
    {
        sound_[QStringLiteral("playableProperties")] = QJsonObject{
            {QStringLiteral("presence"), QStringLiteral("0000000000002000")},
            {QStringLiteral("spatial.attenuationRef"), reference},
        };
    }

    QJsonObject sound_;
    QJsonObject attenuation_;
    QJsonObject other_;
};

QJsonObject harkenJsonIgnoringVolatileKeys(const QByteArray& data)
{
    QJsonObject root = QJsonDocument::fromJson(data).object();
    QJsonObject content = root[QStringLiteral("content")].toObject();
    QJsonObject metadata = content[QStringLiteral("metadata")].toObject();
    metadata.remove(QStringLiteral("modifiedDate"));
    content[QStringLiteral("metadata")] = metadata;
    root[QStringLiteral("content")] = content;
    return root;
}

void fillChassis(ProjectItem& item, float seed, const ProjectItem& bus,
                 const ProjectItem& sendBus)
{
    auto* m = item.comp<VMixNode>();
    ASSERT_NE(m, nullptr) << item.name.toStdString();
    m->volume = -seed;
    m->pitch = seed * 0.5f;
    if (item.type == ProjectItem::Type::Bus) {
        m->outputBusVolume = -seed * 0.25f;
        m->sends.push_back(VNX::Send{sendBus.objectID, 1.0f});
        markPlayablePropertiesPresent(
            item, {harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume,
                   harkenplayableproperties::PlayablePropertyIdV1::Sends});
    } else {
        item.fieldGroups.mainOutput.shared = {
            VNX::FieldGroupMode::Local,
            VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(bus.guid), -seed * 0.25f}};
        item.fieldGroups.send.shared = {
            VNX::FieldGroupMode::Local,
            VNX::SendFieldGroup{{{VNX::toFieldGroupGuid(sendBus.guid), 0.0f}}}};
    }
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(item.type), QUuid::createUuid());
    binding.guid = QUuid::createUuid();
    binding.extensions.insert(QStringLiteral("id"), static_cast<int>(seed));
    ASSERT_TRUE(m->bindings.replace({binding}));
    VNX::EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.parameters = VNX::EffectChainSemantics::defaultParameters(effect.type);
    effect.extensions.insert(QStringLiteral("slot"), 1);
    ASSERT_TRUE(m->effects.replace({effect}));
    markPlayablePropertiesPresent(
        item,
        {harkenplayableproperties::PlayablePropertyIdV1::Volume,
         harkenplayableproperties::PlayablePropertyIdV1::Pitch});
}

void expectChassis(const ProjectItem& item, float seed, uint32_t outputBusId,
                   uint32_t sendBusId)
{
    const auto* m = item.comp<VMixNode>();
    ASSERT_NE(m, nullptr) << item.name.toStdString();
    EXPECT_FLOAT_EQ(m->volume, -seed) << item.name.toStdString();
    EXPECT_FLOAT_EQ(m->pitch, seed * 0.5f);
    if (item.type != ProjectItem::Type::Bus)
        EXPECT_EQ(m->outputBusId, outputBusId);
    EXPECT_FLOAT_EQ(m->outputBusVolume, -seed * 0.25f);
    ASSERT_EQ(m->sends.size(), 1u) << item.name.toStdString();
    EXPECT_EQ(m->sends[0].targetBusID, sendBusId);
    EXPECT_FLOAT_EQ(m->sends[0].sendLevel, 1.0f);
    ASSERT_EQ(m->bindings.size(), 1);
    EXPECT_EQ(m->bindings.toJson()[0].toObject()[QStringLiteral("id")].toInt(),
              static_cast<int>(seed));
    ASSERT_EQ(m->effects.size(), 1);
    EXPECT_EQ(m->effects[0].toObject()[QStringLiteral("slot")].toInt(), 1);
    const auto* sp = item.comp<VSpatial>();
    if (item.type == ProjectItem::Type::Bus) {
        EXPECT_EQ(sp, nullptr) << item.name.toStdString();
    } else {
        ASSERT_NE(sp, nullptr) << item.name.toStdString();
    }
}

const ProjectItem* findDescendant(const ProjectItem& root, const QString& name)
{
    if (root.name == name) return &root;
    for (const auto& child : root.children)
        if (const ProjectItem* hit = findDescendant(*child, name))
            return hit;
    return nullptr;
}

const ProjectItem* findInBucket(const std::vector<ProjectFileBinding>& bucket,
                                const QString& name)
{
    for (const auto& binding : bucket)
        if (binding.rootItem)
            if (const ProjectItem* hit = findDescendant(*binding.rootItem, name))
                return hit;
    return nullptr;
}

}

TEST_F(SpatialReferenceSerializationTest, LegacyReferencesAreRejectedBeforeIdentityPreparation)
{
    for (int scenario = 0; scenario < 3; ++scenario) {
        SCOPED_TRACE(scenario);
        setLegacyReference({{QStringLiteral("attenuation"), QStringLiteral("Cliff")}});
        if (scenario != 1)
            setLegacyReference({{QStringLiteral("attenuationGuid"),
                                 attenuation_.value(QStringLiteral("guid"))}});
        if (scenario == 2) setCanonicalReference(canonicalReference(attenuation_));
        const QJsonObject original = sound_;
        for (const bool audioBranch : {false, true}) {
            SCOPED_TRACE(audioBranch);
            VNX::ProjectSerializer serializer;
            EXPECT_FALSE(serializer.prepareStandaloneReferenceRead(
                QJsonArray{sound_, attenuation_, other_}));
            ProjectItem loaded(QStringLiteral("Unchanged"), ProjectItem::Type::Sound);
            EXPECT_FALSE(audioBranch ? serializer.audioBranchFromJson(sound_, loaded)
                                     : serializer.projectItemFromJson(sound_, loaded));
            EXPECT_FALSE(serializer.lastError().isEmpty());
            EXPECT_EQ(loaded.name, QStringLiteral("Unchanged"));
            EXPECT_EQ(sound_, original);
        }
        QJsonObject components = sound_.value(QStringLiteral("components")).toObject();
        QJsonObject spatial = components.value(QStringLiteral("spatial")).toObject();
        spatial.remove(QStringLiteral("attenuationGuid"));
        components[QStringLiteral("spatial")] = spatial;
        sound_[QStringLiteral("components")] = components;
    }
}

TEST_F(SpatialReferenceSerializationTest, LegacyAndPerFieldReferencesCannotBypassGroupStorage)
{
    setLegacyReference({{QStringLiteral("attenuationGuid"),
                         attenuation_.value(QStringLiteral("guid"))}});
    for (const QJsonValue canonical : {QJsonValue(canonicalReference(other_)),
                                      QJsonValue(QJsonValue::Null)}) {
        setCanonicalReference(canonical);
        VNX::ProjectSerializer serializer;
        EXPECT_FALSE(serializer.prepareStandaloneReferenceRead(
            QJsonArray{sound_, attenuation_, other_}));
        ProjectItem loaded(QStringLiteral("Unchanged"), ProjectItem::Type::Sound);
        EXPECT_FALSE(serializer.audioBranchFromJson(sound_, loaded));
        EXPECT_FALSE(serializer.lastError().isEmpty());
        EXPECT_EQ(loaded.name, QStringLiteral("Unchanged"));
    }
}

TEST_F(SpatialReferenceSerializationTest, PreservesExplicitNoneInLocalRatingGroup)
{
    QJsonObject groups = sound_.value(QStringLiteral("fieldGroups")).toObject();
    QJsonObject shared = groups.value(QStringLiteral("shared")).toObject();
    shared[QStringLiteral("spatialRating")] = QJsonObject{
        {QStringLiteral("inherit"), false},
        {QStringLiteral("values"), QJsonObject{
            {QStringLiteral("spatialPanning"), 0.0},
            {QStringLiteral("spatialIs3DEnabled"), false},
            {QStringLiteral("spatialAttenuationRef"), QStringLiteral("00000000-0000-0000-0000-000000000000")},
            {QStringLiteral("spatialAttenuationDistanceScaling"), 1.0},
        }},
    };
    groups[QStringLiteral("shared")] = shared;
    sound_[QStringLiteral("fieldGroups")] = groups;
    VNX::ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareStandaloneReferenceRead(QJsonArray{sound_}));
    ProjectItem loaded;
    ASSERT_TRUE(serializer.projectItemFromJson(sound_, loaded));
    EXPECT_EQ(loaded.fieldGroups.spatialRating.shared.mode, VNX::FieldGroupMode::Local);
    ASSERT_TRUE(loaded.fieldGroups.spatialRating.shared.localHistory);
    EXPECT_EQ(loaded.fieldGroups.spatialRating.shared.localHistory->spatialAttenuationRef,
              harkenprojectschema::AuthoringGuid{});
    EXPECT_EQ(loaded.playablePropertyPresence.bits & VNX::kFieldGroupPropertyMask, 0u);
    EXPECT_EQ(serializer.projectItemToJson(loaded).value(QStringLiteral("fieldGroups")), groups);
}

TEST_F(SpatialReferenceSerializationTest, RejectsUnresolvedAndAmbiguousLegacyReferences)
{
    for (int scenario = 0; scenario < 3; ++scenario) {
        SCOPED_TRACE(scenario);
        QJsonObject legacy{{QStringLiteral("attenuation"), QStringLiteral("Cliff")}};
        if (scenario == 1)
            legacy[QStringLiteral("attenuationGuid")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        setLegacyReference(legacy);
        QJsonArray roots{sound_};
        if (scenario == 2) {
            other_[QStringLiteral("name")] = QStringLiteral("Cliff");
            roots.append(attenuation_);
            roots.append(other_);
        }
        VNX::ProjectSerializer serializer;
        EXPECT_FALSE(serializer.prepareStandaloneReferenceRead(roots));
        ProjectItem loaded;
        EXPECT_FALSE(serializer.projectItemFromJson(sound_, loaded));
        EXPECT_TRUE(serializer.lastError().contains(QStringLiteral("Retired field group authoring field")));
        QJsonObject components = sound_.value(QStringLiteral("components")).toObject();
        QJsonObject spatial = components.value(QStringLiteral("spatial")).toObject();
        spatial.remove(QStringLiteral("attenuationGuid"));
        components[QStringLiteral("spatial")] = spatial;
        sound_[QStringLiteral("components")] = components;
    }
}

TEST(ProjectSerialization, RetiresAttenuationCurveAcrossDefinitionsClipsAndPlatforms)
{
    ProjectItem container(QStringLiteral("Container"), ProjectItem::Type::Container);
    container.guid = QUuid::createUuid();
    container.attachDefaultComponents();
    const auto sound = addChild(container, "Shot", ProjectItem::Type::Sound);
    ProjectItem track(QStringLiteral("Track"), ProjectItem::Type::Track);
    track.guid = QUuid::createUuid();
    track.attachDefaultComponents();
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = sound->guid;
    clip.duration = 100;
    auto* timeline = track.comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);

    VNX::ProjectSerializer writer;
    QJsonObject containerJson = writer.projectItemToJson(container);
    QJsonObject trackJson = writer.projectItemToJson(track);
    ASSERT_FALSE(containerJson.isEmpty());
    ASSERT_FALSE(trackJson.isEmpty());
    const QJsonObject retired{
        {QStringLiteral("presence"), QStringLiteral("0000000000001001")},
        {QStringLiteral("volume"), -3.0},
        {QStringLiteral("spatial.attenuationCurveType"), 2},
        {QStringLiteral("platformOverrides"), QJsonArray{QJsonObject{
            {QStringLiteral("platformId"), HarkenTest::platformIdString("Windows")},
            {QStringLiteral("presence"), QStringLiteral("0000000000001001")},
            {QStringLiteral("volume"), -4.0},
            {QStringLiteral("spatial.attenuationCurveType"), 1},
        }}},
    };
    containerJson[QStringLiteral("playableProperties")] = retired;
    QJsonObject components = trackJson.value(QStringLiteral("components")).toObject();
    QJsonObject trackTimeline = components.value(QStringLiteral("tracktimeline")).toObject();
    QJsonArray clips = trackTimeline.value(QStringLiteral("clips")).toArray();
    ASSERT_EQ(clips.size(), 1);
    QJsonObject clipJson = clips.first().toObject();
    clipJson[QStringLiteral("playableProperties")] = retired;
    clips[0] = clipJson;
    trackTimeline[QStringLiteral("clips")] = clips;
    components[QStringLiteral("tracktimeline")] = trackTimeline;
    trackJson[QStringLiteral("components")] = components;

    VNX::ProjectSerializer reader;
    ASSERT_TRUE(reader.prepareStandaloneReferenceRead(QJsonArray{containerJson, trackJson}));
    ProjectItem loadedContainer;
    ProjectItem loadedTrack;
    ASSERT_TRUE(reader.projectItemFromJson(containerJson, loadedContainer))
        << reader.lastError().toStdString();
    ASSERT_TRUE(reader.audioBranchFromJson(trackJson, loadedTrack))
        << reader.lastError().toStdString();
    EXPECT_EQ(loadedContainer.playablePropertyPresence.bits, 1u);
    EXPECT_FLOAT_EQ(loadedContainer.comp<VMixNode>()->volume, -3.0f);
    ASSERT_EQ(loadedContainer.platformOverrides.rows.size(), 1u);
    EXPECT_EQ(loadedContainer.platformOverrides.rows[0].properties.presence.bits, 1u);
    EXPECT_FLOAT_EQ(std::get<float>(*loadedContainer.platformOverrides.rows[0].properties.value(
                        harkenplayableproperties::PlayablePropertyIdV1::Volume)), -4.0f);
    ASSERT_EQ(loadedContainer.comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 1u);
    const auto occurrence = loadedContainer.comp<VNX::VSelection>()->selections.shared.occurrenceRows()[0];
    EXPECT_EQ(occurrence.childGuid, sound->guid);
    EXPECT_EQ(occurrence.stableOccurrenceOrdinal,
              container.comp<VNX::VSelection>()->selections.shared.occurrenceRows()[0].stableOccurrenceOrdinal);
    EXPECT_EQ(occurrence.weight, 50);
    const auto* loadedTimeline = loadedTrack.comp<VNX::VTrack>();
    ASSERT_NE(loadedTimeline, nullptr);
    ASSERT_EQ(loadedTimeline->clips.items.size(), 1u);
    const auto& loadedClip = loadedTimeline->clips.items[0];
    EXPECT_EQ(loadedClip.clipGuid, clip.clipGuid);
    EXPECT_EQ(loadedClip.targetGuid, clip.targetGuid);
    EXPECT_EQ(loadedClip.stableOccurrenceOrdinal, clip.stableOccurrenceOrdinal);
    EXPECT_EQ(loadedClip.start, clip.start);
    EXPECT_EQ(loadedClip.duration, clip.duration);
    EXPECT_EQ(loadedClip.playableProperties, VNX::PlayablePropertyOccurrenceV1{});
    EXPECT_TRUE(loadedClip.platformOverrides.rows.empty());
    const QByteArray saved = QJsonDocument(QJsonArray{
        reader.projectItemToJson(loadedContainer), reader.projectItemToJson(loadedTrack),
    }).toJson();
    EXPECT_FALSE(saved.contains("attenuationCurveType"));
    EXPECT_FALSE(saved.contains("0000000000001001"));
    EXPECT_TRUE(reader.warnings().isEmpty());
}

TEST_F(SpatialReferenceSerializationTest, RejectsMalformedRetiredCurvePresence)
{
    for (const QString presence : {QStringLiteral("0000000000000000"),
                                   QStringLiteral("0000000000005000")}) {
        sound_[QStringLiteral("playableProperties")] = QJsonObject{
            {QStringLiteral("presence"), presence},
            {QStringLiteral("spatial.attenuationCurveType"), 1},
        };
        VNX::ProjectSerializer serializer;
        ASSERT_TRUE(serializer.prepareStandaloneReferenceRead(QJsonArray{sound_}));
        ProjectItem loaded;
        EXPECT_FALSE(serializer.projectItemFromJson(sound_, loaded));
        EXPECT_TRUE(serializer.lastError().contains(QStringLiteral("invalid value or presence")));
    }
}

TEST(ProjectSerialization, DbConversionBoundaries)
{
    EXPECT_FLOAT_EQ(VMixNode::linearToDb(1.0f), 0.0f);
    EXPECT_FLOAT_EQ(VMixNode::linearToDb(0.0f), -96.0f);
    EXPECT_FLOAT_EQ(VMixNode::linearToDb(-0.5f), -96.0f);
    EXPECT_NEAR(VMixNode::linearToDb(0.5f), -6.0206f, 1e-3f);
    EXPECT_FLOAT_EQ(VMixNode::dbToLinear(0.0f), 1.0f);
    EXPECT_NEAR(VMixNode::dbToLinear(-6.0206f), 0.5f, 1e-4f);
    EXPECT_NEAR(VMixNode::dbToLinear(VMixNode::linearToDb(0.25f)), 0.25f, 1e-5f);
}

TEST(ProjectSerialization, SaveWritesNoInlineMixKeys)
{
    ProjectItem item(QStringLiteral("Shot"), ProjectItem::Type::Sound);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    auto* m = item.comp<VMixNode>();
    ASSERT_NE(m, nullptr);
    m->volume = -9.0f;
    m->pitch = 3.0f;
    m->makeUpGain = 2.0f;
    auto* a = item.comp<VAudio>();
    ASSERT_NE(a, nullptr);
    a->loopCount = 0;
    markPlayablePropertiesPresent(
        item,
        {harkenplayableproperties::PlayablePropertyIdV1::Volume,
         harkenplayableproperties::PlayablePropertyIdV1::Pitch,
         harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain,
         harkenplayableproperties::PlayablePropertyIdV1::LoopCount});

    VNX::ProjectSerializer serializer;
    const QJsonObject saved = serializer.projectItemToJson(item);
    EXPECT_FALSE(saved["components"].toObject()["audio"].toObject().contains("loopEnabled"));
    EXPECT_EQ(saved["components"].toObject()["audio"].toObject()["loopCount"].toInt(), 0);
    EXPECT_FALSE(saved.contains(QLatin1String("volume")));
    EXPECT_FALSE(saved.contains(QLatin1String("pitch")));
    EXPECT_FALSE(saved.contains(QLatin1String("outputBusId")));
    EXPECT_FALSE(saved.contains(QLatin1String("makeUpGain")));
    EXPECT_FALSE(saved.contains(QLatin1String("loopEnabled")));
    EXPECT_FALSE(saved.contains(QLatin1String("positioning")));
    EXPECT_FALSE(saved.contains(QLatin1String("overrideBits")));

    ProjectItem reloaded;
    ASSERT_TRUE(serializer.projectItemFromJson(saved, reloaded));
    const auto* m2 = reloaded.comp<VMixNode>();
    ASSERT_NE(m2, nullptr);
    EXPECT_FLOAT_EQ(m2->volume, -9.0f);
    EXPECT_FLOAT_EQ(m2->pitch, 3.0f);
    EXPECT_FLOAT_EQ(m2->makeUpGain, 2.0f);
    const auto* a2 = reloaded.comp<VAudio>();
    ASSERT_NE(a2, nullptr);
    EXPECT_EQ(a2->loopCount, 0u);
}

TEST(ProjectSerialization, NewProjectIdentityRemainsStableAcrossSaves)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    ProjectData project = HarkenTest::projectWithPlatforms();
    project.name = QStringLiteral("Identity");
    project.projectPath = QDir(tempDir.path()).filePath(QStringLiteral("Identity.hsp"));
    auto master = std::make_shared<ProjectItem>(QStringLiteral("Master"), ProjectItem::Type::Bus);
    master->guid = QUuid::createUuid();
    master->objectID = 1;
    master->guid = VNX::authoringUuid(harkentest::StudioObjectId(1));
    master->attachDefaultComponents();
    project.busses.push_back({master, QStringLiteral("Busses")});

    VNX::ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareProjectSave(project.projectPath, project))
        << serializer.lastError().toStdString();
    ASSERT_TRUE(serializer.commitProjectSave());

    QFile firstFile(project.projectPath);
    ASSERT_TRUE(firstFile.open(QIODevice::ReadOnly));
    const QString firstId =
        HarkenTest::projectHeader(
            QJsonDocument::fromJson(firstFile.readAll()).object())
            .value(QStringLiteral("id")).toString();
    firstFile.close();
    ASSERT_FALSE(firstId.isEmpty());
    EXPECT_EQ(project.projectId, firstId);

    ASSERT_TRUE(serializer.prepareProjectSave(project.projectPath, project));
    ASSERT_TRUE(serializer.commitProjectSave());

    QFile secondFile(project.projectPath);
    ASSERT_TRUE(secondFile.open(QIODevice::ReadOnly));
    const QString secondId =
        HarkenTest::projectHeader(
            QJsonDocument::fromJson(secondFile.readAll()).object())
            .value(QStringLiteral("id")).toString();
    EXPECT_EQ(secondId, firstId);
    EXPECT_EQ(project.projectId, firstId);
}

TEST(ProjectSerialization, ChassisFieldsRoundtripOnMixNodeKeys)
{
    ProjectItem item(QStringLiteral("Shot"), ProjectItem::Type::Sound);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    auto* m = item.comp<VMixNode>();
    ASSERT_NE(m, nullptr);
    m->bindings = BindingLedger::fromJson(
        QJsonArray{ QJsonObject{{ QStringLiteral("id"), 3 }} });
    m->effects  = QJsonArray{ QJsonObject{{ QStringLiteral("slot"), 0 }} };
    auto* sp = item.comp<VSpatial>();
    ASSERT_NE(sp, nullptr);
    sp->spatializationMix = 0.5f;
    item.fieldGroups.spatialRating.shared = {
        VNX::FieldGroupMode::Local, VNX::SpatialRatingFieldGroup{0.0f, true, {}, 2.0f}};

    VNX::ProjectSerializer serializer;
    const QJsonObject saved = serializer.projectItemToJson(item);
    ASSERT_FALSE(saved.isEmpty()) << serializer.lastError().toStdString();
    const QJsonObject comps = saved["components"].toObject();
    const QJsonObject mix = comps["mixnode"].toObject();
    EXPECT_FALSE(mix.contains(QStringLiteral("attenuation")));
    EXPECT_FALSE(mix.contains(QStringLiteral("attenuationGuid")));
    EXPECT_EQ(mix["bindings"].toArray(), m->bindings.toJson());
    EXPECT_EQ(mix["effects"].toArray(), m->effects.toJson());
    const QJsonObject spatial = comps["spatial"].toObject();
    EXPECT_FALSE(spatial.contains(QStringLiteral("attenuation")));
    EXPECT_FALSE(spatial.contains(QStringLiteral("attenuationGuid")));
    EXPECT_FALSE(spatial.contains(QStringLiteral("is3DEnabled")));
    EXPECT_FLOAT_EQ(static_cast<float>(spatial["spatializationMix"].toDouble()), 0.5f);
    EXPECT_FALSE(spatial.contains(QStringLiteral("attenuationDistanceScaling")));
    const QJsonObject playable = saved[QStringLiteral("playableProperties")].toObject();
    EXPECT_EQ(playable[QStringLiteral("presence")].toString(),
              QStringLiteral("0000000000000000"));
    EXPECT_FALSE(playable.contains(QStringLiteral("spatial.is3DEnabled")));
    EXPECT_FALSE(playable.contains(QStringLiteral("spatial.attenuationDistanceScaling")));
    const QJsonObject audio = comps["audio"].toObject();
    EXPECT_FALSE(audio.contains(QStringLiteral("attenuation")));
    EXPECT_FALSE(audio.contains(QStringLiteral("attenuationGuid")));
    EXPECT_FALSE(audio.contains(QStringLiteral("is3DEnabled")));
    EXPECT_FALSE(audio.contains(QStringLiteral("spatializationMix")));
    EXPECT_FALSE(audio.contains(QStringLiteral("attenuationDistanceScaling")));
    EXPECT_FALSE(audio.contains(QStringLiteral("modulatorBindings")));

    ProjectItem reloaded;
    ASSERT_TRUE(serializer.projectItemFromJson(saved, reloaded));
    const auto* m2 = reloaded.comp<VMixNode>();
    ASSERT_NE(m2, nullptr);
    EXPECT_EQ(m2->bindings.toJson(), m->bindings.toJson());
    EXPECT_EQ(m2->effects.toJson(), m->effects.toJson());
    const auto* sp2 = reloaded.comp<VSpatial>();
    ASSERT_NE(sp2, nullptr);
    EXPECT_FLOAT_EQ(sp2->spatializationMix, 0.5f);
    EXPECT_EQ(reloaded.fieldGroups, item.fieldGroups);
}

TEST(ProjectSerialization, TransitionTableIsOptionalAndNonDefaultRoundtrips)
{
    ProjectItem container(QStringLiteral("Container"),
                          ProjectItem::Type::Container);
    container.guid = QUuid::createUuid();
    container.attachDefaultComponents();
    VNX::ProjectSerializer serializer;

    const QJsonObject canonical = serializer.projectItemToJson(container);
    const QJsonObject canonicalSelection = canonical[QStringLiteral("components")]
        .toObject()[QStringLiteral("selection")].toObject();
    EXPECT_FALSE(canonicalSelection.contains(QStringLiteral("transitions")));
    ProjectItem legacy;
    ASSERT_TRUE(serializer.projectItemFromJson(canonical, legacy));
    ASSERT_NE(legacy.comp<VNX::VSelection>(), nullptr);
    EXPECT_EQ(legacy.comp<VNX::VSelection>()->transitions,
              VNX::canonicalTransitionTable());

    VNX::TransitionRuleDef override = VNX::canonicalTransitionRule();
    override.source = QUuid::createUuid();
    override.target = QUuid::createUuid();
    ASSERT_TRUE(container.comp<VNX::VSelection>()->transitions.addRule(override));
    QJsonObject saved = serializer.projectItemToJson(container);
    QJsonObject selection = saved[QStringLiteral("components")]
        .toObject()[QStringLiteral("selection")].toObject();
    ASSERT_TRUE(selection.contains(QStringLiteral("transitions")));
    QJsonObject transitions = selection[QStringLiteral("transitions")].toObject();
    transitions[QStringLiteral("futureField")] = 42;
    selection[QStringLiteral("transitions")] = transitions;
    QJsonObject components = saved[QStringLiteral("components")].toObject();
    components[QStringLiteral("selection")] = selection;
    saved[QStringLiteral("components")] = components;

    ProjectItem reloaded;
    ASSERT_TRUE(serializer.projectItemFromJson(saved, reloaded));
    EXPECT_EQ(reloaded.comp<VNX::VSelection>()->transitions,
              container.comp<VNX::VSelection>()->transitions);
}

TEST(ProjectSerialization, InvalidTransitionTableRejectsItemLoad)
{
    ProjectItem container(QStringLiteral("Container"),
                          ProjectItem::Type::Container);
    container.guid = QUuid::createUuid();
    container.attachDefaultComponents();
    VNX::TransitionRuleDef override = VNX::canonicalTransitionRule();
    override.source = QUuid::createUuid();
    override.target = QUuid::createUuid();
    ASSERT_TRUE(container.comp<VNX::VSelection>()->transitions.addRule(override));

    VNX::ProjectSerializer serializer;
    QJsonObject saved = serializer.projectItemToJson(container);
    QJsonObject components = saved[QStringLiteral("components")].toObject();
    QJsonObject selection = components[QStringLiteral("selection")].toObject();
    QJsonObject transitions = selection[QStringLiteral("transitions")].toObject();
    QJsonArray rules = transitions[QStringLiteral("rules")].toArray();
    rules.append(rules.last());
    transitions[QStringLiteral("rules")] = rules;
    selection[QStringLiteral("transitions")] = transitions;
    components[QStringLiteral("selection")] = selection;
    saved[QStringLiteral("components")] = components;

    ProjectItem rejected;
    EXPECT_FALSE(serializer.projectItemFromJson(saved, rejected));
}

TEST(ProjectSerialization, UnknownTypeIntIsRejected)
{
    VNX::ProjectSerializer serializer;
    for (int raw : {25, 99, -1}) {
        QJsonObject json{{QStringLiteral("name"), QStringLiteral("Ghost")},
                         {QStringLiteral("type"), raw}};
        ProjectItem item;
        EXPECT_FALSE(serializer.projectItemFromJson(json, item)) << raw;
    }
    QJsonObject missingType{{QStringLiteral("name"), QStringLiteral("Ghost")}};
    ProjectItem item;
    EXPECT_FALSE(serializer.projectItemFromJson(missingType, item));
}

TEST(ProjectSerialization, UnknownParentRefKindIsRejected)
{
    VNX::ProjectSerializer serializer;
    for (const char* kind : {"audio_layer", "audio_bus", "nope", ""}) {
        QJsonObject json{
            {QStringLiteral("name"), QStringLiteral("Ghost")},
            {QStringLiteral("type"), static_cast<int>(ProjectItem::Type::Sound)},
            {QStringLiteral("parent_ref"),
             QJsonObject{{QStringLiteral("kind"), QLatin1String(kind)}}}};
        ProjectItem item;
        EXPECT_FALSE(serializer.projectItemFromJson(json, item)) << kind;
    }
}

TEST(ProjectSerialization, ParentRefKindWordsRoundtrip)
{
    ParentRef ref;
    ref.kind = ParentRef::Kind::Item;
    ref.guid = QUuid::createUuid();
    EXPECT_EQ(ref.toJson()[QStringLiteral("kind")].toString(), QStringLiteral("item"));

    ref.kind = ParentRef::Kind::Bus;
    EXPECT_EQ(ref.toJson()[QStringLiteral("kind")].toString(), QStringLiteral("bus"));

    const auto busBack = ParentRef::fromJson(ref.toJson());
    ASSERT_TRUE(busBack.has_value());
    EXPECT_EQ(busBack->kind, ParentRef::Kind::Bus);
    EXPECT_EQ(busBack->guid, ref.guid);

    ref.kind = ParentRef::Kind::Item;
    EXPECT_EQ(ref.toJson()[QStringLiteral("kind")].toString(), QStringLiteral("item"));

    const auto itemBack = ParentRef::fromJson(ref.toJson());
    ASSERT_TRUE(itemBack.has_value());
    EXPECT_EQ(itemBack->kind, ParentRef::Kind::Item);
    EXPECT_EQ(itemBack->guid, ref.guid);
}

TEST(ProjectSerialization, BusPackHeaderRejectsOldTypeWord)
{
    VNX::ProjectSerializer serializer;
    ProjectItem bucketParent(QStringLiteral("Busses"), ProjectItem::Type::None);
    std::unordered_map<uint32_t, uint32_t> edges;
    QJsonObject json{{QStringLiteral("type"), QStringLiteral("AudioBus")},
                     {QStringLiteral("componentData"), QJsonObject{}}};
    EXPECT_EQ(serializer.busPackFromJson(json, bucketParent, edges), nullptr);
    EXPECT_TRUE(bucketParent.children.empty());
}

TEST(ProjectSerialization, SavedProjectContainsNoRetiredTypeWords)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6WordFace");
    config.location = tempDir.path();
    ASSERT_TRUE(VNX::ProjectManager::instance().createProject(config));
    ASSERT_TRUE(VNX::ProjectManager::instance().saveProject());

    const QString text = projectFilesText(tempDir.path());
    ASSERT_FALSE(text.isEmpty());
    for (const char* word : {"AudioLayer", "AudioBus", "audio_layer",
                             "audio_bus", "Audio Layer", "Audio Layers"})
        EXPECT_FALSE(text.contains(QLatin1String(word))) << word;

    VNX::ProjectManager::instance().closeProject();
}

TEST(ProjectSerialization, LoadingRetiresSignalGraphAndSavingClearsEveryLocation)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6NoSignalGraph");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(pm.createProject(config));

    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("Amb_Field"),
                                       ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* rain = pm.createItem(layer, QStringLiteral("Amb_Rain"),
                                      ProjectItem::Type::Sound);
    ASSERT_NE(rain, nullptr);
    ProjectItem* birds = pm.createItem(layer, QStringLiteral("Birds"),
                                       ProjectItem::Type::Container);
    ASSERT_NE(birds, nullptr);
    ProjectItem* chirp = pm.createItem(birds, QStringLiteral("Chirp_A"),
                                      ProjectItem::Type::Sound);
    ASSERT_NE(chirp, nullptr);
    ProjectItem* track = pm.createItem(layer, QStringLiteral("Track"),
                                      ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    ProjectItem* piece = pm.createItem(track, QStringLiteral("Piece"),
                                      ProjectItem::Type::Piece);
    ASSERT_NE(piece, nullptr);
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = piece->guid;
    clip.duration = 100;
    auto* timeline = track->comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    timeline->clips.items.clear();
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);
    VNX::EffectEntry gain;
    gain.guid = QUuid::createUuid();
    gain.type = VNX::EffectType::Gain;
    gain.parameters = {-2.0f};
    ASSERT_TRUE(rain->comp<VMixNode>()->effects.replace({gain}));
    const QJsonArray effects = rain->comp<VMixNode>()->effects.toJson();
    std::vector<QUuid> definitions;
    for (const auto* item : {pm.getMasterBusItem(), layer, rain, birds, chirp, track, piece}) {
        ASSERT_NE(item, nullptr);
        definitions.push_back(item->guid);
    }
    const QUuid rainGuid = rain->guid;
    const QUuid birdsGuid = birds->guid;
    const QUuid trackGuid = track->guid;
    const QString projectPath = pm.currentProject()->projectPath;
    const QString projectRoot = QDir(tempDir.path()).filePath(config.name);

    ASSERT_TRUE(pm.saveProject());
    const QString text = projectFilesText(projectRoot);
    ASSERT_FALSE(text.isEmpty());
    EXPECT_FALSE(text.contains(QLatin1String("signalGraph")));
    const auto original = projectFileBytes(projectRoot);
    pm.closeProject();

    const QJsonObject cleaned{
        {QStringLiteral("presence"), QStringLiteral("0000000000000001")},
        {QStringLiteral("volume"), -3.0},
        {QStringLiteral("platformOverrides"), QJsonArray{
            QJsonObject{
                {QStringLiteral("platformId"), HarkenTest::platformIdString("Windows")},
                {QStringLiteral("presence"), QStringLiteral("0000000000000001")},
                {QStringLiteral("volume"), -4.0},
            },
            QJsonObject{
                {QStringLiteral("platformId"), HarkenTest::platformIdString("Android")},
                {QStringLiteral("presence"), QStringLiteral("0000000000000000")},
            },
        }},
    };
    QJsonObject retired = cleaned;
    retired[QStringLiteral("presence")] = QStringLiteral("0000000000008001");
    retired[QStringLiteral("signalGraph")] = QJsonArray{QJsonObject{
        {QStringLiteral("nodeId"), 1}, {QStringLiteral("effectType"), 2},
    }};
    QJsonArray platforms = retired.value(QStringLiteral("platformOverrides")).toArray();
    for (qsizetype index = 0; index < platforms.size(); ++index) {
        QJsonObject row = platforms[index].toObject();
        row[QStringLiteral("presence")] = index == 0
            ? QStringLiteral("0000000000008001") : QStringLiteral("0000000000008000");
        row[QStringLiteral("signalGraph")] = QJsonArray{};
        platforms[index] = row;
    }
    retired[QStringLiteral("platformOverrides")] = platforms;

    std::function<QJsonValue(QJsonValue, const QJsonObject&, bool)> withProperties =
        [&](QJsonValue value, const QJsonObject& properties, bool discardOccurrences) -> QJsonValue {
            if (value.isObject()) {
                QJsonObject object = value.toObject();
                const auto replacement = discardOccurrences && object.contains(QStringLiteral("stableOccurrenceOrdinal"))
                    ? canonicalPlayableProperties() : properties;
                for (auto it = object.begin(); it != object.end(); ++it)
                    it.value() = it.key() == QLatin1String("playableProperties")
                        ? QJsonValue(replacement) : withProperties(it.value(), properties, discardOccurrences);
                return object;
            }
            if (value.isArray()) {
                QJsonArray array = value.toArray();
                for (qsizetype index = 0; index < array.size(); ++index)
                    array[index] = withProperties(array[index], properties, discardOccurrences);
                return array;
            }
            return value;
        };
    QMap<QString, QJsonObject> expected;
    for (auto it = original.cbegin(); it != original.cend(); ++it) {
        const QJsonObject document = QJsonDocument::fromJson(it.value()).object();
        expected.insert(it.key(), withProperties(document, cleaned, true).toObject());
        QFile file(QDir(projectRoot).filePath(it.key()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray bytes = QJsonDocument(withProperties(document, retired, false).toObject()).toJson();
        ASSERT_EQ(file.write(bytes), bytes.size());
    }
    ASSERT_TRUE(projectFilesText(projectRoot).contains(QLatin1String("signalGraph")));
    QString loadError;
    ASSERT_TRUE(pm.loadProject(projectPath, false, &loadError)) << loadError.toStdString();
    for (const auto& guid : definitions) {
        const auto* item = pm.findItemByGuid(guid);
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->playablePropertyPresence.bits, 1u);
        EXPECT_FLOAT_EQ(item->comp<VMixNode>()->volume, -3.0f);
        ASSERT_EQ(item->platformOverrides.rows.size(), 2u);
        ASSERT_NE(item->platformOverrides.find(HarkenTest::PlatformIds::Windows), nullptr);
        ASSERT_NE(item->platformOverrides.find(HarkenTest::PlatformIds::Android), nullptr);
        EXPECT_EQ(item->platformOverrides.find(HarkenTest::PlatformIds::Windows)->properties.presence.bits, 1u);
        EXPECT_EQ(item->platformOverrides.find(HarkenTest::PlatformIds::Android)->properties.presence.bits, 0u);
    }
    EXPECT_EQ(pm.findItemByGuid(rainGuid)->comp<VMixNode>()->effects.toJson(), effects);
    const auto& occurrences = pm.findItemByGuid(birdsGuid)->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
    ASSERT_EQ(occurrences.size(), 1u);
    EXPECT_EQ(occurrences[0].weight, 50);
    const auto& clips = pm.findItemByGuid(trackGuid)->comp<VNX::VTrack>()->clips.items;
    ASSERT_EQ(clips.size(), 1u);
    EXPECT_EQ(clips[0].playableProperties, VNX::PlayablePropertyOccurrenceV1{});
    EXPECT_TRUE(clips[0].platformOverrides.rows.empty());
    ASSERT_TRUE(pm.saveProject());
    const auto saved = projectFileBytes(projectRoot);
    ASSERT_EQ(saved.size(), expected.size());
    for (auto it = saved.cbegin(); it != saved.cend(); ++it) {
        SCOPED_TRACE(it.key().toStdString());
        const auto actual = QJsonDocument::fromJson(it.value()).object();
        auto expectedDocument = expected.value(it.key());
        if (it.key().endsWith(QLatin1String(".hsp"))) {
            const auto modifiedDate = actual.value(QStringLiteral("content")).toObject()
                .value(QStringLiteral("metadata")).toObject().value(QStringLiteral("modifiedDate"));
            EXPECT_TRUE(QDateTime::fromString(modifiedDate.toString(), Qt::ISODate).isValid());
            auto content = expectedDocument.value(QStringLiteral("content")).toObject();
            auto metadata = content.value(QStringLiteral("metadata")).toObject();
            metadata.insert(QStringLiteral("modifiedDate"), modifiedDate);
            content.insert(QStringLiteral("metadata"), metadata);
            expectedDocument.insert(QStringLiteral("content"), content);
        }
        EXPECT_EQ(actual, expectedDocument);
    }
    EXPECT_FALSE(projectFilesText(projectRoot).contains(QLatin1String("signalGraph")));
    pm.closeProject();
}

TEST(ProjectSerialization, NineClassAudioTreeRoundtrip)
{
    ProjectItem layer(QStringLiteral("Amb_Field"), ProjectItem::Type::Layer);
    layer.attachDefaultComponents();
    layer.guid = QUuid::createUuid();

    addChild(layer, "Amb_Rain", ProjectItem::Type::Sound);
    auto birds = addChild(layer, "Birds", ProjectItem::Type::Container);
    auto chirp = addChild(*birds, "Chirp_A", ProjectItem::Type::Sound);
    auto seq   = addChild(*birds, "Seq", ProjectItem::Type::Track);
    auto intro = addChild(*seq, "Intro", ProjectItem::Type::Piece);
    auto tail  = addChild(*intro, "Tail", ProjectItem::Type::Track);
    addChild(*tail, "Tail_Hit", ProjectItem::Type::Sound);

    chirp->comp<VMixNode>()->volume = -7.0f;
    intro->comp<VMixNode>()->pitch = 2.0f;
    markPlayablePropertiesPresent(
        *chirp, {harkenplayableproperties::PlayablePropertyIdV1::Volume});
    markPlayablePropertiesPresent(
        *intro, {harkenplayableproperties::PlayablePropertyIdV1::Pitch});

    VNX::ProjectSerializer serializer;

    std::function<QJsonObject(const ProjectItem&)> inlineWindowForm =
        [&](const ProjectItem& node) {
            QJsonObject json = serializer.audioItemToJson(node);
            EXPECT_TRUE(json[QStringLiteral("children")].toArray().isEmpty())
                << node.name.toStdString();
            QJsonArray children;
            for (const auto& child : node.children)
                children.append(inlineWindowForm(*child));
            json[QStringLiteral("children")] = children;
            return json;
        };
    const QJsonObject json = inlineWindowForm(layer);
    ASSERT_FALSE(json.isEmpty());

    ProjectItem reloaded(QString(), ProjectItem::Type::Layer);
    ASSERT_TRUE(serializer.audioItemFromJson(json, reloaded));

    ASSERT_EQ(reloaded.children.size(), 2u);
    const auto& rain2 = reloaded.children[0];
    const auto& birds2 = reloaded.children[1];
    EXPECT_EQ(rain2->type, ProjectItem::Type::Sound);
    ASSERT_EQ(birds2->type, ProjectItem::Type::Container);
    EXPECT_EQ(birds2->parent, &reloaded);

    ASSERT_EQ(birds2->children.size(), 2u);
    const auto& chirp2 = birds2->children[0];
    const auto& seq2 = birds2->children[1];
    EXPECT_EQ(chirp2->type, ProjectItem::Type::Sound);
    EXPECT_FLOAT_EQ(chirp2->comp<VMixNode>()->volume, -7.0f);
    ASSERT_EQ(seq2->type, ProjectItem::Type::Track);

    ASSERT_EQ(seq2->children.size(), 1u);
    const auto& intro2 = seq2->children[0];
    ASSERT_EQ(intro2->type, ProjectItem::Type::Piece);
    EXPECT_FLOAT_EQ(intro2->comp<VMixNode>()->pitch, 2.0f);
    EXPECT_EQ(intro2->parent, seq2.get());

    ASSERT_EQ(intro2->children.size(), 1u);
    const auto& tail2 = intro2->children[0];
    ASSERT_EQ(tail2->type, ProjectItem::Type::Track);
    ASSERT_EQ(tail2->children.size(), 1u);
    EXPECT_EQ(tail2->children[0]->type, ProjectItem::Type::Sound);
    EXPECT_EQ(tail2->children[0]->parent, tail2.get());
}

TEST(ProjectSerialization, AudioBranchGatesIllegalNestingBothSides)
{
    VNX::ProjectSerializer serializer;

    ProjectItem layer(QStringLiteral("L"), ProjectItem::Type::Layer);
    layer.attachDefaultComponents();
    layer.guid = QUuid::createUuid();
    auto sound = addChild(layer, "Shot", ProjectItem::Type::Sound);
    addChild(*sound, "Ghost", ProjectItem::Type::Sound);
    addChild(layer, "Loose", ProjectItem::Type::None);

    const QJsonObject saved = serializer.audioItemToJson(layer);
    EXPECT_TRUE(saved.isEmpty());
    const QJsonObject savedSound = serializer.audioItemToJson(*sound);
    EXPECT_FALSE(savedSound.isEmpty());

    const QJsonObject trackJson{
        {QStringLiteral("name"), QStringLiteral("Bad")},
        {QStringLiteral("type"), static_cast<int>(ProjectItem::Type::Track)},
        {QStringLiteral("guid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("playableProperties"), canonicalPlayableProperties()}};
    const QJsonObject soundJson{
        {QStringLiteral("name"), QStringLiteral("Shot")},
        {QStringLiteral("type"), static_cast<int>(ProjectItem::Type::Sound)},
        {QStringLiteral("guid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("playableProperties"), canonicalPlayableProperties()},
        {QStringLiteral("children"), QJsonArray{trackJson}}};
    const QJsonObject layerJson{
        {QStringLiteral("name"), QStringLiteral("L")},
        {QStringLiteral("type"), static_cast<int>(ProjectItem::Type::Layer)},
        {QStringLiteral("guid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("playableProperties"), canonicalPlayableProperties()},
        {QStringLiteral("children"), QJsonArray{soundJson}}};

    ProjectItem reloaded(QString(), ProjectItem::Type::Layer);
    EXPECT_FALSE(serializer.audioItemFromJson(layerJson, reloaded));
}

TEST(ProjectSerialization, FiveBucketAccessorsAgree)
{
    ProjectData data;
    const auto& buckets = VNX::ItemTypeCatalog::allBuckets();
    ASSERT_EQ(buckets.size(), 5u);

    const QStringList dirs = VNX::ProjectManager::getStandardDirectories();
    ASSERT_EQ(dirs.size(), 5);

    std::set<const void*> distinctMembers;
    for (size_t i = 0; i < buckets.size(); ++i) {
        const QString folder = VNX::ItemTypeCatalog::folderNameOf(buckets[i]);
        EXPECT_EQ(dirs[static_cast<int>(i)], folder);
        auto* member = data.getBucketForFolder(folder);
        ASSERT_NE(member, nullptr) << folder.toStdString();
        distinctMembers.insert(member);

        ProjectFileBinding binding;
        binding.parentFolder = folder;
        member->push_back(std::move(binding));
    }
    EXPECT_EQ(distinctMembers.size(), 5u);
    EXPECT_EQ(data.allFileBindings().size(), 5u);
}

TEST(ProjectSerialization, VariablePackRoundtripAcrossSaveLoad)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6Variables");
    config.location = tempDir.path();
    ASSERT_TRUE(VNX::ProjectManager::instance().createProject(config));

    ProjectData* project = VNX::ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectRoot = QDir(tempDir.path()).filePath(config.name);
    const QString projectPath = project->projectPath;

    auto wind = std::make_shared<ProjectItem>(QStringLiteral("Wind_Intensity"),
                                              ProjectItem::Type::Variable);
    wind->attachDefaultComponents();
    wind->guid = QUuid::createUuid();
    wind->objectID = project->generateObjectID();
    wind->parentRef.kind = ParentRef::Kind::Root;
    auto* vv = wind->comp<VVariable>();
    ASSERT_NE(vv, nullptr);
    vv->kind = VariableKind::Step;
    vv->rangeMin = 5.0f;
    vv->rangeMax = 50.0f;
    vv->defaultValue = 25.0f;
    vv->labels.push_back(VariableLabel{5, QStringLiteral("Calm")});
    vv->labels.push_back(VariableLabel{50, QStringLiteral("Storm")});

    auto drip = std::make_shared<ProjectItem>(QStringLiteral("Cave_Drip"),
                                              ProjectItem::Type::Variable);
    drip->attachDefaultComponents();
    drip->guid = QUuid::createUuid();
    drip->objectID = project->generateObjectID();
    drip->parentRef.kind = ParentRef::Kind::Root;
    auto* folder = VNX::ProjectManager::instance().createFolder(
        nullptr, QStringLiteral("Env"), QStringLiteral("Variables"));
    ASSERT_NE(folder, nullptr);

    for (auto& item : {wind, drip}) {
        ProjectFileBinding binding;
        binding.parentFolder = QLatin1String(VNX::ItemTypeCatalog::kVariablesFolderName);
        binding.rootItem = item;
        project->setObjectFileLocation(item.get(), item == drip ? folder : nullptr, {});
        project->variables.push_back(std::move(binding));
    }

    ASSERT_TRUE(VNX::ProjectManager::instance().saveProject());
    EXPECT_TRUE(QFile::exists(
        QDir(projectRoot).filePath(objectFilePath(wind.get()))));
    EXPECT_TRUE(QFile::exists(
        QDir(projectRoot).filePath(objectFilePath(drip.get()))));

    ASSERT_TRUE(VNX::ProjectManager::instance().loadProject(projectPath));
    project = VNX::ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_EQ(project->variables.size(), 2u);

    const ProjectFileBinding* windBinding = nullptr;
    const ProjectFileBinding* dripBinding = nullptr;
    for (const auto& binding : project->variables) {
        if (!binding.rootItem) continue;
        if (binding.rootItem->name == QStringLiteral("Wind_Intensity"))
            windBinding = &binding;
        else if (binding.rootItem->name == QStringLiteral("Cave_Drip"))
            dripBinding = &binding;
    }
    ASSERT_NE(windBinding, nullptr);
    ASSERT_NE(dripBinding, nullptr);

    EXPECT_EQ(windBinding->rootItem->type, ProjectItem::Type::Variable);
    const auto* vv2 = windBinding->rootItem->comp<VVariable>();
    ASSERT_NE(vv2, nullptr);
    EXPECT_EQ(vv2->kind, VariableKind::Step);
    EXPECT_FLOAT_EQ(vv2->rangeMin, 5.0f);
    EXPECT_FLOAT_EQ(vv2->rangeMax, 50.0f);
    EXPECT_FLOAT_EQ(vv2->defaultValue, 25.0f);
    EXPECT_FALSE(vv2->toJson().contains(QStringLiteral("scope")));
    ASSERT_EQ(vv2->labels.size(), 2u);
    EXPECT_EQ(vv2->labels[0].anchor, 5);
    EXPECT_EQ(vv2->labels[0].name, QStringLiteral("Calm"));
    EXPECT_EQ(vv2->labels[1].anchor, 50);
    EXPECT_EQ(vv2->labels[1].name, QStringLiteral("Storm"));

    EXPECT_EQ(project->itemFolderPath(dripBinding->rootItem.get()), QStringLiteral("Env"));

    int variableBindings = 0;
    for (const auto* binding :
         static_cast<const ProjectData*>(project)->allFileBindings()) {
        if (binding->rootItem
            && binding->rootItem->type == ProjectItem::Type::Variable)
            ++variableBindings;
    }
    EXPECT_EQ(variableBindings, 2);

    VNX::ProjectManager::instance().closeProject();
}

TEST(ProjectSerialization, VariableContractFieldsRoundtripCreateItemSaveLoad)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6VariableContract");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectItem* item = pm.createItem(nullptr, QStringLiteral("Drive"),
                                      ProjectItem::Type::Variable);
    ASSERT_NE(item, nullptr);
    auto* variable = item->comp<VVariable>();
    ASSERT_NE(variable, nullptr);
    variable->kind = VariableKind::Smooth;
    variable->rangeMin = -6.0f;
    variable->rangeMax = 6.0f;
    variable->defaultValue = 1.5f;
    variable->writers = 3;
    variable->unit = QStringLiteral("dB");
    variable->precision = 2;
    const QUuid guid = item->guid;
    const QString projectPath = pm.currentProject()->projectPath;
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.loadProject(projectPath));
    ProjectItem* loaded = pm.findItemByGuid(guid);
    ASSERT_NE(loaded, nullptr);
    const auto* reloaded = loaded->comp<VVariable>();
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->kind, VariableKind::Smooth);
    EXPECT_FALSE(reloaded->toJson().contains(QStringLiteral("scope")));
    EXPECT_FLOAT_EQ(reloaded->rangeMin, -6.0f);
    EXPECT_FLOAT_EQ(reloaded->rangeMax, 6.0f);
    EXPECT_FLOAT_EQ(reloaded->defaultValue, 1.5f);
    EXPECT_EQ(reloaded->writers, 3u);
    EXPECT_EQ(reloaded->unit, QStringLiteral("dB"));
    EXPECT_EQ(reloaded->precision, 2);
    pm.closeProject();
}

TEST(ProjectSerialization, ActionQuantizeTimingsRoundtripThroughProjectManagerSave)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    const auto closeProject = qScopeGuard([&pm]() { pm.closeProject(); });

    VNX::ProjectConfig config;
    config.name = QStringLiteral("ActionQuantizeTimings");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));

    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("Layer"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* sound = pm.createItem(
        layer, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ProjectItem* action = pm.createItem(
        nullptr, QStringLiteral("Quantized"), ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    const QUuid actionGuid = action->guid;

    auto* component = action->comp<VNX::VAction>();
    ASSERT_NE(component, nullptr);
    const std::vector<VNX::QuantizeTiming> timings = {
        VNX::QuantizeTiming::Immediate,
        VNX::QuantizeTiming::NextBeat,
        VNX::QuantizeTiming::NextBar,
        VNX::QuantizeTiming::NextMarker,
        VNX::QuantizeTiming::NaturalEnd,
        VNX::QuantizeTiming::CurrentLoopEnd,
        VNX::QuantizeTiming::EntryMarker,
        VNX::QuantizeTiming::ExitMarker,
    };
    for (const auto timing : timings) {
        VNX::VActionStep step;
        step.type = VNX::VActionStep::Type::Stop;
        step.targetObjectID = sound->objectID;
        step.targetName = sound->name;
        step.quantize.timing = timing;
        if (timing == VNX::QuantizeTiming::NextBeat
            || timing == VNX::QuantizeTiming::NextBar
            || timing == VNX::QuantizeTiming::NextMarker
            || timing == VNX::QuantizeTiming::EntryMarker) {
            step.quantize.referenceObjectID = sound->objectID;
            step.quantize.referenceName = sound->name;
        }
        component->steps.push_back(step);
    }

    QString saveError;
    QObject observer;
    QObject::connect(&pm, &VNX::ProjectManager::errorOccurred, &observer,
                     [&](const QString& error) { saveError = error; });
    const QString projectPath = pm.currentProject()->projectPath;
    ASSERT_TRUE(pm.saveProject()) << saveError.toStdString();
    ASSERT_TRUE(pm.loadProject(projectPath));

    const ProjectItem* loadedAction = pm.findItemByGuid(actionGuid);
    ASSERT_NE(loadedAction, nullptr);
    const auto* loadedComponent = loadedAction->comp<VNX::VAction>();
    ASSERT_NE(loadedComponent, nullptr);
    ASSERT_EQ(loadedComponent->steps.size(), timings.size());
    for (size_t index = 0; index < timings.size(); ++index)
        EXPECT_EQ(loadedComponent->steps[index].quantize.timing, timings[index]);
}

TEST(ProjectSerialization, PhysicalFoldersPreserveVariableAndAttenuationFiles)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PhysicalFolders");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(pm.createProject(config));
    const QString projectPath = pm.currentProject()->projectPath;
    auto* variableFolder = pm.createFolder(nullptr, QStringLiteral("Env"), QStringLiteral("Variables"));
    auto* attenuationFolder = pm.createFolder(nullptr, QStringLiteral("Cliffs"), QStringLiteral("Attenuations"));
    ASSERT_NE(variableFolder, nullptr);
    ASSERT_NE(attenuationFolder, nullptr);
    auto* wind = pm.createItem(nullptr, QStringLiteral("Wind_Intensity"), ProjectItem::Type::Variable,
                               QStringLiteral("Env"));
    auto* attenuation = pm.createItem(nullptr, QStringLiteral("Att_Far"), ProjectItem::Type::Attenuation,
                                      QStringLiteral("Cliffs"));
    ASSERT_NE(wind, nullptr);
    ASSERT_NE(attenuation, nullptr);
    const QUuid windGuid = wind->guid;
    const QUuid attenuationGuid = attenuation->guid;
    wind->comp<VVariable>()->rangeMin = 5.0f;
    wind->comp<VVariable>()->rangeMax = 50.0f;
    wind->comp<VVariable>()->defaultValue = 25.0f;
    attenuation->comp<VNX::VAttenuation>()->maxDistance = 42.0f;
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.loadProject(projectPath));
    wind = pm.findItemByGuid(windGuid);
    attenuation = pm.findItemByGuid(attenuationGuid);
    ASSERT_NE(wind, nullptr);
    ASSERT_NE(attenuation, nullptr);
    EXPECT_EQ(wind->parent, nullptr);
    EXPECT_EQ(attenuation->parent, nullptr);
    EXPECT_EQ(pm.currentProject()->itemFolderPath(wind), QStringLiteral("Env"));
    EXPECT_EQ(pm.currentProject()->itemFolderPath(attenuation), QStringLiteral("Cliffs"));
    EXPECT_FLOAT_EQ(wind->comp<VVariable>()->defaultValue, 25.0f);
    EXPECT_FLOAT_EQ(attenuation->comp<VNX::VAttenuation>()->maxDistance, 42.0f);
    pm.closeProject();
}

TEST(ProjectSerialization, FolderTransactionCacheIsExcludedFromProjectDocuments)
{
    ensureApp();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("FolderUndoCache");
    config.location = directory.path();
    auto& manager = VNX::ProjectManager::instance();
    const auto closeProject = qScopeGuard([&manager]() { manager.closeProject(); });
    ASSERT_TRUE(manager.createProject(config));
    auto* action = manager.createItem(nullptr, QStringLiteral("Play_Retained"), ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    const QUuid guid = action->guid;
    const QString projectPath = manager.currentProject()->projectPath;
    const QDir projectDirectory(QFileInfo(projectPath).absolutePath());
    ASSERT_TRUE(manager.saveProject());

    const QString cachePath = projectDirectory.filePath(QStringLiteral(".cache/folder-operations/operation"));
    ASSERT_TRUE(QDir().mkpath(cachePath));
    ASSERT_TRUE(QFile::copy(projectDirectory.filePath(objectFilePath(action)),
                           QDir(cachePath).filePath(QStringLiteral("Duplicate.vp"))));
    QFile invalid(QDir(cachePath).filePath(QStringLiteral("Invalid.vp")));
    ASSERT_TRUE(invalid.open(QIODevice::WriteOnly));
    invalid.write("not a project document");
    invalid.close();

    ASSERT_TRUE(manager.loadProject(projectPath));
    EXPECT_NE(manager.findItemByGuid(guid), nullptr);
    EXPECT_EQ(manager.currentProject()->actions.size(), 1u);
}

// Gold byte face remade for the VSpatial migration (phase-1 rulings U2/U3,
// case-B direct key move): the attenuation reference pair moved from the
// "mixnode" component key and the three spatial fields moved from "audio"
// into the new "spatial" component key, carried by the content four types
// plus Layer and never by Bus. Saved bytes therefore legitimately changed;
// the save -> load -> resave byte-stability contract itself is unchanged.
TEST(ProjectSerialization, GoldNineClassFiveBucketRoundtripResaveByteStable)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6Gold");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    const auto closeProject = qScopeGuard([&pm]() { pm.closeProject(); });
    ASSERT_TRUE(pm.createProject(config));
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectRoot = QDir(tempDir.path()).filePath(config.name);
    const QString projectPath = project->projectPath;

    ProjectItem* master = pm.getMasterBusItem();
    ASSERT_NE(master, nullptr);
    ProjectItem* bus = pm.createItem(master, QStringLiteral("Mix_World"),
                                     ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    auto* vb = bus->comp<VNX::VBus>();
    ASSERT_NE(vb, nullptr);
    vb->pan = 0.25f;
    vb->listenerRelativeRouting = true;

    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("Amb_Field"),
                                       ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* rain = pm.createItem(layer, QStringLiteral("Amb_Rain"),
                                      ProjectItem::Type::Sound);
    ASSERT_NE(rain, nullptr);
    ProjectItem* birds = pm.createItem(layer, QStringLiteral("Birds"),
                                       ProjectItem::Type::Container);
    ASSERT_NE(birds, nullptr);
    ProjectItem* chirp = pm.createItem(birds, QStringLiteral("Chirp_A"),
                                       ProjectItem::Type::Sound);
    ASSERT_NE(chirp, nullptr);
    ProjectItem* seq = pm.createItem(birds, QStringLiteral("Seq"),
                                     ProjectItem::Type::Track);
    ASSERT_NE(seq, nullptr);
    ProjectItem* intro = pm.createItem(seq, QStringLiteral("Intro"),
                                       ProjectItem::Type::Piece);
    ASSERT_NE(intro, nullptr);
    ProjectItem* tail = pm.createItem(intro, QStringLiteral("Tail"),
                                      ProjectItem::Type::Track);
    ASSERT_NE(tail, nullptr);
    ProjectItem* hit = pm.createItem(tail, QStringLiteral("Tail_Hit"),
                                     ProjectItem::Type::Sound);
    ASSERT_NE(hit, nullptr);

    auto* rainAudio = rain->comp<VAudio>();
    ASSERT_NE(rainAudio, nullptr);
    rainAudio->loopCount = 0;
    auto* rainMix = rain->comp<VMixNode>();
    ASSERT_NE(rainMix, nullptr);
    rainMix->makeUpGain = 2.5f;
    rain->fieldGroups.spatialRating.shared = {
        VNX::FieldGroupMode::Local, VNX::SpatialRatingFieldGroup{0.0f, true, {}, 1.0f}};
    markPlayablePropertiesPresent(
        *rain,
        {harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain,
         harkenplayableproperties::PlayablePropertyIdV1::LoopCount});
    auto* birdsSelection = birds->comp<VNX::VSelection>();
    ASSERT_NE(birdsSelection, nullptr);
    birdsSelection->mode = VNX::SelectionMode::Sequence;

    const struct { ProjectItem* item; float seed; } chassisPlan[] = {
        {layer, 2.0f}, {rain, 4.0f},  {birds, 6.0f}, {chirp, 8.0f},
        {seq, 10.0f},  {intro, 12.0f}, {tail, 14.0f}, {hit, 16.0f},
        {bus, 18.0f}};
    for (const auto& entry : chassisPlan)
        fillChassis(*entry.item, entry.seed, *bus,
                    entry.item->type == ProjectItem::Type::Bus ? *bus : *master);

    ProjectItem* swing = pm.createItem(nullptr, QStringLiteral("Play_Swing"),
                                       ProjectItem::Type::Action,
                                       QStringLiteral("Combat"));
    ASSERT_NE(swing, nullptr);
    auto* action = swing->comp<VNX::VAction>();
    ASSERT_NE(action, nullptr);
    VNX::VActionStep step;
    step.type = VNX::VActionStep::Type::Stop;
    step.targetObjectID = rain->objectID;
    step.targetName = QStringLiteral("Amb_Rain");
    step.delay = 0.25f;
    step.fadeTime = 0.5f;
    step.quantize.referenceObjectID = rain->objectID;
    step.quantize.referenceName = QStringLiteral("Amb_Rain");
    step.quantize.timing = VNX::QuantizeTiming::ExitMarker;
    step.quantize.ifSilent = VNX::QuantizeIfSilent::Drop;
    action->steps.push_back(step);
    VNX::VActionStep setValue;
    setValue.type = VNX::VActionStep::Type::SetVariable;
    setValue.targetObjectID = 0;
    setValue.targetName = QStringLiteral("Wind_Intensity");
    setValue.value = 17.0f;
    action->steps.push_back(setValue);
    VNX::VActionStep postAction;
    postAction.type = VNX::VActionStep::Type::PostAction;
    postAction.targetObjectID = swing->objectID;
    postAction.targetName = QStringLiteral("Play_Swing");
    action->steps.push_back(postAction);
    VNX::VActionStep reset;
    reset.type = VNX::VActionStep::Type::Reset;
    reset.targetObjectID = birds->objectID;
    reset.targetName = QStringLiteral("Birds");
    reset.resetScope = VNX::ResetScope::Global;
    action->steps.push_back(reset);

    ProjectItem* att = pm.createItem(nullptr, QStringLiteral("Att_Cliff"),
                                     ProjectItem::Type::Attenuation);
    ASSERT_NE(att, nullptr);
    auto* attComp = att->comp<VNX::VAttenuation>();
    ASSERT_NE(attComp, nullptr);
    attComp->maxDistance = 42.0f;
    att->parentRef = ParentRef{};
    att->parentRef.kind = ParentRef::Kind::Root;
    ASSERT_NE(bus->runtimeId(), harkentest::StudioObjectId(0u));
    ASSERT_NE(att->runtimeId(), harkentest::StudioObjectId(0u));
    for (const auto& entry : chassisPlan) {
        if (entry.item->type == ProjectItem::Type::Bus) continue;
        auto& rating = entry.item->fieldGroups.spatialRating.shared;
        auto payload = rating.localHistory.value_or(VNX::SpatialRatingFieldGroup{});
        payload.spatialAttenuationRef = VNX::toFieldGroupGuid(att->guid);
        rating = {VNX::FieldGroupMode::Local, payload};
    }
    const auto setOccurrenceMix = [&](VNX::PlayablePropertyOccurrenceV1* value) {
        ASSERT_NE(value, nullptr);
        value->presence.Set(
            harkenplayableproperties::PlayablePropertyIdV1::Volume);
        value->presence.Set(
            harkenplayableproperties::PlayablePropertyIdV1::Pitch);
        ASSERT_TRUE(value->setValue(
            harkenplayableproperties::PlayablePropertyIdV1::Volume, -3.0f));
        ASSERT_TRUE(value->setValue(
            harkenplayableproperties::PlayablePropertyIdV1::Pitch, 0.5f));
    };
    auto rows = birds->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
    auto occurrence = std::find_if(
        rows.begin(),
        rows.end(),
        [&](const auto& row) { return row.childGuid == chirp->guid; });
    ASSERT_NE(occurrence, rows.end());
    occurrence->weight = 73;
    ASSERT_TRUE(birds->comp<VNX::VSelection>()->selections.shared.setOccurrenceRows(rows));
    const auto expectedOccurrence = *occurrence;
    auto* timeline = seq->comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    timeline->clips.items.clear();
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = intro->guid;
    clip.start = 0;
    clip.duration = 1;
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    setOccurrenceMix(&clip.playableProperties);
    timeline->clips.items.push_back(std::move(clip));
    const auto expectedClip = timeline->clips.items.front();

    auto wind = std::make_shared<ProjectItem>(QStringLiteral("Wind_Intensity"),
                                              ProjectItem::Type::Variable);
    wind->attachDefaultComponents();
    wind->guid = QUuid::createUuid();
    wind->objectID = project->generateObjectID();
    wind->objectPath = QStringLiteral("\\Wind_Intensity");
    wind->parentRef.kind = ParentRef::Kind::Root;
    auto* vv = wind->comp<VVariable>();
    ASSERT_NE(vv, nullptr);
    vv->kind = VariableKind::Step;
    vv->rangeMin = 5.0f;
    vv->rangeMax = 50.0f;
    vv->defaultValue = 25.0f;
    vv->labels.push_back(VariableLabel{5, QStringLiteral("Calm")});
    vv->labels.push_back(VariableLabel{50, QStringLiteral("Storm")});
    {
        ProjectFileBinding binding;
        binding.parentFolder =
            QLatin1String(VNX::ItemTypeCatalog::kVariablesFolderName);
        binding.rootItem = wind;
        project->variables.push_back(std::move(binding));
    }
    action->steps[1].targetObjectID = wind->objectID;

    QString saveError;
    QObject observer;
    QObject::connect(&pm, &VNX::ProjectManager::errorOccurred, &observer,
                     [&](const QString& error) { saveError = error; });
    ASSERT_TRUE(pm.saveProject()) << saveError.toStdString();
    const auto snap1 = projectFileBytes(projectRoot);
    ASSERT_FALSE(snap1.isEmpty());
    const QString savedText = projectFilesText(projectRoot);
    EXPECT_FALSE(savedText.contains(QStringLiteral("\"objectID\"")));
    EXPECT_FALSE(savedText.contains(QStringLiteral("\"nextObjectID\"")));
    for (const auto* item : {layer, rain, birds, chirp, seq, intro, tail, hit})
        EXPECT_TRUE(snap1.contains(objectFilePath(item))) << item->name.toStdString();
    {
        const QJsonObject birdsPack = QJsonDocument::fromJson(
            snap1.value(objectFilePath(birds))).object();
        const QJsonObject birdsContent = HarkenTest::projectContent(birdsPack);
        const QJsonObject birdsRef =
            birdsContent[QStringLiteral("parentRef")].toObject();
        EXPECT_EQ(birdsRef[QStringLiteral("kind")].toString(),
                  QStringLiteral("item"));
        EXPECT_EQ(birdsRef[QStringLiteral("id")].toString(),
                  layer->guid.toString(QUuid::WithoutBraces));
        EXPECT_EQ(birdsContent[QStringLiteral("order")].toInt(-1), 1);
        EXPECT_TRUE(birdsContent
                        [QStringLiteral("children")].toArray().isEmpty());

        const QJsonObject chirpPack = QJsonDocument::fromJson(
            snap1.value(objectFilePath(chirp))).object();
        const QJsonObject chirpRef =
            HarkenTest::projectContent(chirpPack)
                [QStringLiteral("parentRef")].toObject();
        EXPECT_EQ(chirpRef[QStringLiteral("kind")].toString(),
                  QStringLiteral("item"));
        EXPECT_EQ(chirpRef[QStringLiteral("id")].toString(),
                  birds->guid.toString(QUuid::WithoutBraces));
        EXPECT_EQ(HarkenTest::projectContent(chirpPack)
                      [QStringLiteral("order")].toInt(-1), 0);
    }
    EXPECT_TRUE(snap1.contains(objectFilePath(att)));
    EXPECT_TRUE(snap1.contains(objectFilePath(wind.get())));
    bool busFileSeen = false;
    bool actionFileSeen = false;
    for (const QString& key : snap1.keys()) {
        busFileSeen |= key.startsWith(QStringLiteral("Busses/"))
                       && key.endsWith(QStringLiteral(".vp"));
        actionFileSeen |= key.startsWith(QStringLiteral("Actions/"))
                          && key == objectFilePath(swing);
    }
    EXPECT_TRUE(busFileSeen);
    EXPECT_TRUE(actionFileSeen);

    ASSERT_TRUE(pm.loadProject(projectPath));
    project = pm.currentProject();
    ASSERT_NE(project, nullptr);

    const ProjectItem* layer2 = findInBucket(project->layers,
                                             QStringLiteral("Amb_Field"));
    ASSERT_NE(layer2, nullptr);
    EXPECT_EQ(layer2->type, ProjectItem::Type::Layer);
    ASSERT_EQ(layer2->children.size(), 2u);
    EXPECT_EQ(layer2->children[0]->type, ProjectItem::Type::Sound);
    ASSERT_EQ(layer2->children[1]->type, ProjectItem::Type::Container);
    const ProjectItem* birds2 = layer2->children[1].get();
    ASSERT_EQ(birds2->children.size(), 2u);
    EXPECT_EQ(birds2->children[0]->type, ProjectItem::Type::Sound);
    ASSERT_EQ(birds2->children[1]->type, ProjectItem::Type::Track);
    const ProjectItem* seq2 = birds2->children[1].get();
    ASSERT_EQ(seq2->children.size(), 1u);
    ASSERT_EQ(seq2->children[0]->type, ProjectItem::Type::Piece);
    const ProjectItem* intro2 = seq2->children[0].get();
    ASSERT_EQ(intro2->children.size(), 1u);
    ASSERT_EQ(intro2->children[0]->type, ProjectItem::Type::Track);
    const ProjectItem* tail2 = intro2->children[0].get();
    ASSERT_EQ(tail2->children.size(), 1u);
    EXPECT_EQ(tail2->children[0]->type, ProjectItem::Type::Sound);

    const ProjectItem* bus2 = findInBucket(project->busses,
                                           QStringLiteral("Mix_World"));
    ASSERT_NE(bus2, nullptr);
    EXPECT_EQ(bus2->type, ProjectItem::Type::Bus);
    const auto* vb2 = bus2->comp<VNX::VBus>();
    ASSERT_NE(vb2, nullptr);
    EXPECT_FLOAT_EQ(vb2->pan, 0.25f);
    EXPECT_TRUE(vb2->listenerRelativeRouting);
    const ProjectItem* master2 = pm.getMasterBusItem();
    ASSERT_NE(master2, nullptr);

    const struct { const char* name; float seed; } expectedChassis[] = {
        {"Amb_Field", 2.0f}, {"Amb_Rain", 4.0f}, {"Birds", 6.0f},
        {"Chirp_A", 8.0f},   {"Seq", 10.0f},     {"Intro", 12.0f},
        {"Tail", 14.0f},     {"Tail_Hit", 16.0f}};
    for (const auto& entry : expectedChassis) {
        const ProjectItem* item = findDescendant(*layer2,
                                                 QLatin1String(entry.name));
        ASSERT_NE(item, nullptr) << entry.name;
        expectChassis(*item, entry.seed, bus2->objectID, master2->objectID);
    }
    expectChassis(*bus2, 18.0f, bus2->objectID, bus2->objectID);

    const ProjectItem* rain2 = findDescendant(*layer2,
                                              QStringLiteral("Amb_Rain"));
    ASSERT_NE(rain2, nullptr);
    const auto* rainAudio2 = rain2->comp<VAudio>();
    ASSERT_NE(rainAudio2, nullptr);
    EXPECT_EQ(rainAudio2->loopCount, 0u);
    const auto* rainMix2 = rain2->comp<VMixNode>();
    ASSERT_NE(rainMix2, nullptr);
    EXPECT_FLOAT_EQ(rainMix2->makeUpGain, 2.5f);
    const auto* rainSpatial2 = rain2->comp<VSpatial>();
    ASSERT_NE(rainSpatial2, nullptr);
    EXPECT_TRUE(rainSpatial2->is3DEnabled);
    const auto* birdsSelection2 = birds2->comp<VNX::VSelection>();
    ASSERT_NE(birdsSelection2, nullptr);
    EXPECT_EQ(birdsSelection2->mode, VNX::SelectionMode::Sequence);

    const ProjectItem* swing2 = findInBucket(project->actions,
                                             QStringLiteral("Play_Swing"));
    ASSERT_NE(swing2, nullptr);
    EXPECT_EQ(swing2->type, ProjectItem::Type::Action);
    EXPECT_EQ(project->itemFolderPath(swing2), QStringLiteral("Combat"));
    const auto* action2 = swing2->comp<VNX::VAction>();
    ASSERT_NE(action2, nullptr);
    ASSERT_EQ(action2->steps.size(), 4u);
    EXPECT_EQ(action2->steps[0].type, VNX::VActionStep::Type::Stop);
    EXPECT_EQ(action2->steps[0].targetObjectID, rain2->objectID);
    EXPECT_EQ(action2->steps[0].targetName, QStringLiteral("Amb_Rain"));
    EXPECT_FLOAT_EQ(action2->steps[0].delay, 0.25f);
    EXPECT_FLOAT_EQ(action2->steps[0].fadeTime, 0.5f);
    EXPECT_EQ(action2->steps[0].quantize.referenceObjectID, rain2->objectID);
    EXPECT_EQ(action2->steps[0].quantize.referenceName, QStringLiteral("Amb_Rain"));
    EXPECT_EQ(action2->steps[0].quantize.timing,
              VNX::QuantizeTiming::ExitMarker);
    EXPECT_EQ(action2->steps[0].quantize.ifSilent, VNX::QuantizeIfSilent::Drop);
    EXPECT_EQ(action2->steps[1].type, VNX::VActionStep::Type::SetVariable);
    EXPECT_FLOAT_EQ(action2->steps[1].value, 17.0f);
    EXPECT_EQ(action2->steps[2].type, VNX::VActionStep::Type::PostAction);
    EXPECT_EQ(action2->steps[3].type, VNX::VActionStep::Type::Reset);
    EXPECT_EQ(action2->steps[3].resetScope, VNX::ResetScope::Global);

    const ProjectItem* att2 = findInBucket(project->attenuationItems,
                                            QStringLiteral("Att_Cliff"));
    ASSERT_NE(att2, nullptr);
    EXPECT_EQ(att2->type, ProjectItem::Type::Attenuation);
    const auto* attComp2 = att2->comp<VNX::VAttenuation>();
    ASSERT_NE(attComp2, nullptr);
    EXPECT_EQ(attComp2->hostItem()->name, QStringLiteral("Att_Cliff"));
    EXPECT_FLOAT_EQ(attComp2->maxDistance, 42.0f);
    for (const auto& entry : expectedChassis) {
        const ProjectItem* item = findDescendant(*layer2,
                                                 QLatin1String(entry.name));
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->comp<VMixNode>()->mainOutputBusRef,
                  bus2->runtimeId());
        EXPECT_EQ(item->comp<VSpatial>()->attenuationRef,
                  att2->runtimeId());
    }
    const auto rows2 = birds2->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
    const auto occurrence2 = std::find_if(
        rows2.cbegin(),
        rows2.cend(),
        [&](const auto& row) { return row.childGuid == birds2->children[0]->guid; });
    ASSERT_NE(occurrence2, rows2.cend());
    EXPECT_EQ(occurrence2->childGuid, expectedOccurrence.childGuid);
    EXPECT_EQ(occurrence2->stableOccurrenceOrdinal, expectedOccurrence.stableOccurrenceOrdinal);
    EXPECT_EQ(occurrence2->weight, expectedOccurrence.weight);
    const auto* timeline2 = seq2->comp<VNX::VTrack>();
    ASSERT_NE(timeline2, nullptr);
    ASSERT_EQ(timeline2->clips.items.size(), 1u);
    EXPECT_EQ(timeline2->clips.items[0].clipGuid, expectedClip.clipGuid);
    EXPECT_EQ(timeline2->clips.items[0].targetGuid, expectedClip.targetGuid);
    EXPECT_EQ(timeline2->clips.items[0].stableOccurrenceOrdinal, expectedClip.stableOccurrenceOrdinal);
    EXPECT_EQ(timeline2->clips.items[0].start, expectedClip.start);
    EXPECT_EQ(timeline2->clips.items[0].duration, expectedClip.duration);
    EXPECT_EQ(timeline2->clips.items[0].playableProperties, VNX::PlayablePropertyOccurrenceV1{});
    EXPECT_TRUE(timeline2->clips.items[0].platformOverrides.rows.empty());

    const ProjectItem* wind2 = findInBucket(project->variables,
                                            QStringLiteral("Wind_Intensity"));
    ASSERT_NE(wind2, nullptr);
    EXPECT_EQ(wind2->type, ProjectItem::Type::Variable);
    const auto* vv2 = wind2->comp<VVariable>();
    ASSERT_NE(vv2, nullptr);
    EXPECT_EQ(vv2->kind, VariableKind::Step);
    EXPECT_FLOAT_EQ(vv2->defaultValue, 25.0f);
    EXPECT_FALSE(vv2->toJson().contains(QStringLiteral("scope")));
    ASSERT_EQ(vv2->labels.size(), 2u);
    EXPECT_EQ(vv2->labels[1].name, QStringLiteral("Storm"));

    ASSERT_TRUE(pm.saveProject());
    const auto snap2 = projectFileBytes(projectRoot);
    ASSERT_EQ(snap1.keys(), snap2.keys());
    for (auto it = snap1.begin(); it != snap1.end(); ++it) {
        if (it.key().endsWith(QStringLiteral(".hsp"))) {
            const QJsonObject first = harkenJsonIgnoringVolatileKeys(it.value());
            const QJsonObject second =
                harkenJsonIgnoringVolatileKeys(snap2.value(it.key()));
            EXPECT_EQ(first.keys(), second.keys());
            for (const QString& key : first.keys())
                EXPECT_EQ(first[key], second[key])
                    << key.toStdString() << ": "
                    << QJsonDocument(QJsonObject{{key, first[key]}})
                           .toJson().toStdString()
                    << " vs "
                    << QJsonDocument(QJsonObject{{key, second[key]}})
                           .toJson().toStdString();
        } else {
            EXPECT_EQ(it.value(), snap2.value(it.key()))
                << it.key().toStdString() << "\n=== first ===\n"
                << it.value().toStdString() << "\n=== second ===\n"
                << snap2.value(it.key()).toStdString();
        }
    }
}

TEST(ProjectSerialization, ContentRootsAtLayersBucketRootRoundtripResaveByteStable)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6ContentRoots");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    const auto closeProject = qScopeGuard([&pm]() { pm.closeProject(); });
    ASSERT_TRUE(pm.createProject(config));
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectRoot = QDir(tempDir.path()).filePath(config.name);
    const QString projectPath = project->projectPath;

    ProjectItem* shot = pm.createItem(nullptr, QStringLiteral("Root_Shot"),
                                      ProjectItem::Type::Sound);
    ASSERT_NE(shot, nullptr);
    ProjectItem* grp = pm.createItem(nullptr, QStringLiteral("Root_Grp"),
                                     ProjectItem::Type::Container);
    ASSERT_NE(grp, nullptr);
    ProjectItem* grpShot = pm.createItem(grp, QStringLiteral("Root_Grp_Shot"),
                                         ProjectItem::Type::Sound);
    ASSERT_NE(grpShot, nullptr);
    ProjectItem* trk = pm.createItem(nullptr, QStringLiteral("Root_Trk"),
                                     ProjectItem::Type::Track);
    ASSERT_NE(trk, nullptr);
    ProjectItem* trkPc = pm.createItem(trk, QStringLiteral("Root_Trk_Pc"),
                                       ProjectItem::Type::Piece);
    ASSERT_NE(trkPc, nullptr);
    ProjectItem* pc = pm.createItem(nullptr, QStringLiteral("Root_Pc"),
                                    ProjectItem::Type::Piece);
    ASSERT_NE(pc, nullptr);
    ProjectItem* pcTrk = pm.createItem(pc, QStringLiteral("Root_Pc_Trk"),
                                       ProjectItem::Type::Track);
    ASSERT_NE(pcTrk, nullptr);

    shot->comp<VMixNode>()->volume = -3.0f;
    grpShot->comp<VMixNode>()->pitch = 1.5f;
    markPlayablePropertiesPresent(
        *shot, {harkenplayableproperties::PlayablePropertyIdV1::Volume});
    markPlayablePropertiesPresent(
        *grpShot, {harkenplayableproperties::PlayablePropertyIdV1::Pitch});

    ASSERT_TRUE(pm.saveProject());
    const auto snap1 = projectFileBytes(projectRoot);
    const struct { ProjectItem* item; const char* typeWord; } packs[] = {
        {shot, "\"type\": \"Sound\""},
        {grp, "\"type\": \"Container\""},
        {grpShot, "\"type\": \"Sound\""},
        {trk, "\"type\": \"Track\""},
        {trkPc, "\"type\": \"Piece\""},
        {pc, "\"type\": \"Piece\""},
        {pcTrk, "\"type\": \"Track\""},
    };
    for (const auto& pack : packs) {
        const QString file = objectFilePath(pack.item);
        ASSERT_TRUE(snap1.contains(file)) << file.toStdString();
        EXPECT_TRUE(snap1.value(file).contains(pack.typeWord)) << file.toStdString();
    }
    {
        const QJsonObject grpShotPack = QJsonDocument::fromJson(
            snap1.value(objectFilePath(grpShot))).object();
        const QJsonObject grpShotRef =
            HarkenTest::projectContent(grpShotPack)
                [QStringLiteral("parentRef")].toObject();
        EXPECT_EQ(grpShotRef[QStringLiteral("kind")].toString(),
                  QStringLiteral("item"));
        EXPECT_EQ(grpShotRef[QStringLiteral("id")].toString(),
                  grp->guid.toString(QUuid::WithoutBraces));
        const QJsonObject grpPack = QJsonDocument::fromJson(
            snap1.value(objectFilePath(grp))).object();
        const QJsonObject grpData = HarkenTest::projectContent(grpPack);
        ASSERT_TRUE(grpData.contains(QStringLiteral("children")));
        EXPECT_TRUE(grpData[QStringLiteral("children")].toArray().isEmpty());
    }

    ASSERT_TRUE(pm.loadProject(projectPath));
    project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_EQ(project->layers.size(), 4u);

    const struct { const char* name; ProjectItem::Type type; } roots[] = {
        {"Root_Shot", ProjectItem::Type::Sound},
        {"Root_Grp", ProjectItem::Type::Container},
        {"Root_Trk", ProjectItem::Type::Track},
        {"Root_Pc", ProjectItem::Type::Piece},
    };
    for (const auto& entry : roots) {
        const ProjectFileBinding* binding = nullptr;
        for (const auto& b : project->layers) {
            if (b.rootItem && b.rootItem->name == QLatin1String(entry.name))
                binding = &b;
        }
        ASSERT_NE(binding, nullptr) << entry.name;
        EXPECT_EQ(binding->rootItem->type, entry.type) << entry.name;
        EXPECT_EQ(binding->rootItem->parent, nullptr) << entry.name;
        EXPECT_EQ(binding->rootItem->parentRef.kind, ParentRef::Kind::Root)
            << entry.name;
    }

    const ProjectItem* shot2 = findInBucket(project->layers,
                                            QStringLiteral("Root_Shot"));
    ASSERT_NE(shot2, nullptr);
    EXPECT_FLOAT_EQ(shot2->comp<VMixNode>()->volume, -3.0f);
    const ProjectItem* grp2 = findInBucket(project->layers,
                                           QStringLiteral("Root_Grp"));
    ASSERT_NE(grp2, nullptr);
    ASSERT_EQ(grp2->children.size(), 1u);
    EXPECT_EQ(grp2->children[0]->type, ProjectItem::Type::Sound);
    EXPECT_FLOAT_EQ(grp2->children[0]->comp<VMixNode>()->pitch, 1.5f);
    const ProjectItem* trk2 = findInBucket(project->layers,
                                           QStringLiteral("Root_Trk"));
    ASSERT_NE(trk2, nullptr);
    ASSERT_EQ(trk2->children.size(), 1u);
    EXPECT_EQ(trk2->children[0]->type, ProjectItem::Type::Piece);
    const ProjectItem* pc2 = findInBucket(project->layers,
                                          QStringLiteral("Root_Pc"));
    ASSERT_NE(pc2, nullptr);
    ASSERT_EQ(pc2->children.size(), 1u);
    EXPECT_EQ(pc2->children[0]->type, ProjectItem::Type::Track);

    ASSERT_TRUE(pm.saveProject());
    const auto snap2 = projectFileBytes(projectRoot);
    ASSERT_EQ(snap1.keys(), snap2.keys());
    for (auto it = snap1.begin(); it != snap1.end(); ++it) {
        if (it.key().endsWith(QStringLiteral(".hsp"))) {
            EXPECT_EQ(harkenJsonIgnoringVolatileKeys(it.value()),
                      harkenJsonIgnoringVolatileKeys(snap2.value(it.key())))
                << it.key().toStdString();
        } else {
            EXPECT_EQ(it.value(), snap2.value(it.key()))
                << it.key().toStdString() << "\n=== first ===\n"
                << it.value().toStdString() << "\n=== second ===\n"
                << snap2.value(it.key()).toStdString();
        }
    }
}

TEST(ProjectSerialization, PerObjectLayoutWritesOneFilePerObjectWithItemParentRefs)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6PerObject");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(pm.createProject(config));
    const QString projectRoot = QDir(tempDir.path()).filePath(config.name);

    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("Po_Layer"),
                                       ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* grp = pm.createItem(layer, QStringLiteral("Po_Grp"),
                                     ProjectItem::Type::Container);
    ASSERT_NE(grp, nullptr);
    ProjectItem* seq = pm.createItem(grp, QStringLiteral("Po_Seq"),
                                     ProjectItem::Type::Track);
    ASSERT_NE(seq, nullptr);
    ProjectItem* pc = pm.createItem(seq, QStringLiteral("Po_Pc"),
                                    ProjectItem::Type::Piece);
    ASSERT_NE(pc, nullptr);
    ProjectItem* tail = pm.createItem(pc, QStringLiteral("Po_Tail"),
                                      ProjectItem::Type::Track);
    ASSERT_NE(tail, nullptr);
    ProjectItem* hit = pm.createItem(tail, QStringLiteral("Po_Hit"),
                                     ProjectItem::Type::Sound);
    ASSERT_NE(hit, nullptr);
    ProjectItem* rain = pm.createItem(layer, QStringLiteral("Po_Rain"),
                                      ProjectItem::Type::Sound);
    ASSERT_NE(rain, nullptr);

    ASSERT_TRUE(pm.saveProject());
    const auto files = projectFileBytes(projectRoot);

    const struct {
        const char* file;
        const ProjectItem* item;
        const ProjectItem* parent;
        int order;
    } packs[] = {
        {"Layers/Po_Layer.vp", layer, nullptr, 0},
        {"Layers/Po_Grp.vp",   grp,   layer,   0},
        {"Layers/Po_Seq.vp",   seq,   grp,     0},
        {"Layers/Po_Pc.vp",    pc,    seq,     0},
        {"Layers/Po_Tail.vp",  tail,  pc,      0},
        {"Layers/Po_Hit.vp",   hit,   tail,    0},
        {"Layers/Po_Rain.vp",  rain,  layer,   1},
    };
    for (const auto& entry : packs) {
        const QString file = objectFilePath(entry.item);
        ASSERT_TRUE(files.contains(file)) << file.toStdString();
        const QJsonObject pack = QJsonDocument::fromJson(
            files.value(file)).object();
        const QJsonObject content = HarkenTest::projectContent(pack);
        EXPECT_EQ(content[QStringLiteral("guid")].toString(),
                  entry.item->guid.toString(QUuid::WithoutBraces)) << entry.file;
        EXPECT_EQ(content[QStringLiteral("order")].toInt(-1), entry.order)
            << entry.file;
        const QJsonObject ref = content[QStringLiteral("parentRef")].toObject();
        if (entry.parent) {
            EXPECT_EQ(ref[QStringLiteral("kind")].toString(),
                      QStringLiteral("item")) << entry.file;
            EXPECT_EQ(ref[QStringLiteral("id")].toString(),
                      entry.parent->guid.toString(QUuid::WithoutBraces))
                << entry.file;
        } else {
            EXPECT_EQ(ref[QStringLiteral("kind")].toString(),
                      QStringLiteral("root")) << entry.file;
        }
        ASSERT_TRUE(content.contains(QStringLiteral("children"))) << entry.file;
        EXPECT_TRUE(content[QStringLiteral("children")].toArray().isEmpty())
            << entry.file;
    }

    pm.closeProject();
}

TEST(ProjectSerialization, WindowInlinePackIsRejectedWithoutChangingDisk)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6Window");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(pm.createProject(config));
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectRoot = QDir(tempDir.path()).filePath(config.name);
    const QString projectPath = project->projectPath;
    ASSERT_TRUE(pm.saveProject());
    pm.closeProject();
    const QString projectId = HarkenTest::projectHeader(
        QJsonDocument::fromJson(projectFileBytes(projectRoot).value(
            QFileInfo(projectPath).fileName())).object())
                                  .value(QStringLiteral("id")).toString();

    const QString rootGuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString subGuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString grpGuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString shotGuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString subShotGuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces);

    const QJsonObject rootRef{{QStringLiteral("kind"), QStringLiteral("root")}};
    const QJsonObject layerRef{{QStringLiteral("kind"), QStringLiteral("item")},
                               {QStringLiteral("id"), rootGuid}};
    auto inlineNode = [](const QString& name, const QString& type,
                         const QString& guid,
                         const QJsonObject& parentRef,
                         const QJsonArray& children) {
        QJsonObject node{{QStringLiteral("name"), name},
                         {QStringLiteral("type"), type},
                         {QStringLiteral("guid"), guid},
                         {QStringLiteral("parentRef"), parentRef},
                         {QStringLiteral("playableProperties"),
                          canonicalPlayableProperties()},
                         {QStringLiteral("children"), children},
                         {QStringLiteral("order"), 0}};
        if (type == QStringLiteral("Container")) {
            QJsonArray rows;
            for (qsizetype i = 0; i < children.size(); ++i) {
                const QJsonObject child = children[i].toObject();
                rows.append(QJsonObject{
                    {QStringLiteral("childGuid"),
                     child.value(QStringLiteral("guid"))},
                    {QStringLiteral("stableOccurrenceOrdinal"), i},
                    {QStringLiteral("weight"), 50},
                });
            }
            node[QStringLiteral("components")] = QJsonObject{{"selection", QJsonObject{
                {"mode", 0}, {"selections", QJsonObject{
                    {"shared", QJsonObject{{"occurrenceOrdinalHighWater", children.size()}, {"rows", rows}}},
                    {"platforms", QJsonObject{}}}}}}};
        }
        return node;
    };
    auto writePack = [&projectRoot](const QString& relPath,
                                    const QJsonObject& obj) {
        QFile f(QDir(projectRoot).filePath(relPath));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    };

    writePack(QStringLiteral("Layers/Wf_Root.vp"),
              HarkenTest::projectDocument(
                  QStringLiteral("project.layer"), rootGuid, projectId,
                  inlineNode(QStringLiteral("Wf_Root"), QStringLiteral("Layer"),
                             rootGuid, rootRef,
                             QJsonArray{inlineNode(
                                 QStringLiteral("Wf_Grp"), QStringLiteral("Container"),
                                 grpGuid, rootRef,
                                 QJsonArray{inlineNode(
                                     QStringLiteral("Wf_Shot"), QStringLiteral("Sound"),
                                     shotGuid,
                                     rootRef, QJsonArray{})})})));
    writePack(QStringLiteral("Layers/Wf_Sub.vp"),
              HarkenTest::projectDocument(
                  QStringLiteral("project.layer"), subGuid, projectId,
                  inlineNode(QStringLiteral("Wf_Sub"), QStringLiteral("Layer"),
                             subGuid, layerRef,
                             QJsonArray{inlineNode(
                                 QStringLiteral("Wf_SubShot"), QStringLiteral("Sound"),
                                 subShotGuid,
                                 rootRef, QJsonArray{})})));

    const auto before = projectFileBytes(projectRoot);
    EXPECT_FALSE(pm.loadProject(projectPath));
    EXPECT_EQ(pm.currentProject(), nullptr);
    EXPECT_EQ(projectFileBytes(projectRoot), before);
}

TEST(ProjectSerialization, SiblingOrderSurvivesRoundtripIndependentOfFilenameLex)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6Order");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(pm.createProject(config));
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectRoot = QDir(tempDir.path()).filePath(config.name);
    const QString projectPath = project->projectPath;

    ProjectItem* zRoot = pm.createItem(nullptr, QStringLiteral("Ord_Z_Root"),
                                       ProjectItem::Type::Layer);
    ASSERT_NE(zRoot, nullptr);
    ProjectItem* aRoot = pm.createItem(nullptr, QStringLiteral("Ord_A_Root"),
                                       ProjectItem::Type::Layer);
    ASSERT_NE(aRoot, nullptr);
    ASSERT_NE(pm.createItem(zRoot, QStringLiteral("Ord_B_snd"),
                            ProjectItem::Type::Sound), nullptr);
    ASSERT_NE(pm.createItem(zRoot, QStringLiteral("Ord_A_snd"),
                            ProjectItem::Type::Sound), nullptr);

    ASSERT_TRUE(pm.saveProject());
    const auto snap1 = projectFileBytes(projectRoot);
    const struct { const ProjectItem* item; int order; } orders[] = {
        {zRoot, 0},
        {aRoot, 1},
        {zRoot->children[0].get(), 0},
        {zRoot->children[1].get(), 1},
    };
    for (const auto& entry : orders) {
        const QString file = objectFilePath(entry.item);
        ASSERT_TRUE(snap1.contains(file)) << file.toStdString();
        const QJsonObject pack = QJsonDocument::fromJson(
            snap1.value(file)).object();
        EXPECT_EQ(HarkenTest::projectContent(pack)
                      [QStringLiteral("order")].toInt(-1), entry.order)
            << file.toStdString();
    }

    ASSERT_TRUE(pm.loadProject(projectPath));
    project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_EQ(project->layers.size(), 2u);
    ASSERT_TRUE(project->layers[0].rootItem);
    ASSERT_TRUE(project->layers[1].rootItem);
    EXPECT_EQ(project->layers[0].rootItem->name,
              QStringLiteral("Ord_Z_Root"));
    EXPECT_EQ(project->layers[1].rootItem->name,
              QStringLiteral("Ord_A_Root"));
    const ProjectItem* z2 = project->layers[0].rootItem.get();
    ASSERT_EQ(z2->children.size(), 2u);
    EXPECT_EQ(z2->children[0]->name, QStringLiteral("Ord_B_snd"));
    EXPECT_EQ(z2->children[1]->name, QStringLiteral("Ord_A_snd"));

    ASSERT_TRUE(pm.saveProject());
    const auto snap2 = projectFileBytes(projectRoot);
    ASSERT_EQ(snap1.keys(), snap2.keys());
    for (auto it = snap1.begin(); it != snap1.end(); ++it) {
        if (it.key().endsWith(QStringLiteral(".hsp"))) {
            EXPECT_EQ(harkenJsonIgnoringVolatileKeys(it.value()),
                      harkenJsonIgnoringVolatileKeys(snap2.value(it.key())))
                << it.key().toStdString();
        } else {
            EXPECT_EQ(it.value(), snap2.value(it.key()))
                << it.key().toStdString();
        }
    }

    pm.closeProject();
}

TEST(ProjectSerialization, RejectsOrphanContentPackWithoutChangingDisk)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6Orphan");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(pm.createProject(config));
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectRoot = QDir(tempDir.path()).filePath(config.name);
    const QString projectPath = project->projectPath;
    ASSERT_TRUE(pm.saveProject());
    pm.closeProject();
    const QJsonObject rootDocument = QJsonDocument::fromJson(
        projectFileBytes(projectRoot).value(QFileInfo(projectPath).fileName())).object();
    const QString projectId = HarkenTest::projectHeader(rootDocument)
                                  .value(QStringLiteral("id")).toString();

    const QString shotGuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString missingGuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject missingRef{{QStringLiteral("kind"), QStringLiteral("item")},
                                 {QStringLiteral("id"), missingGuid}};
    {
        QFile f(QDir(projectRoot).filePath(
            QStringLiteral("Layers/Orph_Shot.vp")));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(HarkenTest::projectDocument(
                    QStringLiteral("project.layer"), shotGuid, projectId,
                    QJsonObject{
                         {QStringLiteral("name"), QStringLiteral("Orph_Shot")},
                         {QStringLiteral("type"), QStringLiteral("Sound")},
                         {QStringLiteral("guid"), shotGuid},
                         {QStringLiteral("parentRef"), missingRef},
                         {QStringLiteral("playableProperties"),
                          canonicalPlayableProperties()},
                         {QStringLiteral("children"), QJsonArray{}},
                         {QStringLiteral("order"), 0},
                    }))
                    .toJson(QJsonDocument::Indented));
    }

    const auto before = projectFileBytes(projectRoot);
    EXPECT_FALSE(pm.loadProject(projectPath));
    EXPECT_EQ(pm.currentProject(), nullptr);
    EXPECT_EQ(projectFileBytes(projectRoot), before);

    pm.closeProject();
}

TEST(ProjectSerialization, RejectsUnsupportedVersionsWithClearErrorAndUntouchedDisk)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6Gate");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(pm.createProject(config));
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectRoot = QDir(tempDir.path()).filePath(config.name);
    const QString projectPath = project->projectPath;
    ASSERT_TRUE(pm.saveProject());
    pm.closeProject();

    QByteArray original;
    {
        QFile f(projectPath);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        original = f.readAll();
    }
    const QJsonObject baseRoot = QJsonDocument::fromJson(original).object();
    ASSERT_EQ(HarkenTest::projectHeader(baseRoot)
                  [QStringLiteral("schemaVersion")].toInt(),
              VNX::kCurrentProjectSchemaVersion);

    const struct {
        const char* label;
        int version;
        bool removeKey;
    } cases[] = {
        {"invalid-version", 0, false},
        {"missing-version-key", 0, true},
        {"next-version", VNX::kCurrentProjectSchemaVersion + 1, false},
    };

    for (const auto& c : cases) {
        QJsonObject tampered = baseRoot;
        QJsonObject header = HarkenTest::projectHeader(tampered);
        if (c.removeKey) {
            header.remove(QStringLiteral("schemaVersion"));
        } else {
            header[QStringLiteral("schemaVersion")] = c.version;
        }
        tampered[QStringLiteral("harkenDocument")] = header;
        {
            QFile f(projectPath);
            ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QJsonDocument(tampered).toJson());
        }
        const auto before = projectFileBytes(projectRoot);

        VNX::ProjectSerializer serializer;
        ProjectData data;
        EXPECT_FALSE(serializer.loadProject(projectPath, data)) << c.label;
        EXPECT_FALSE(serializer.lastError().isEmpty())
            << c.label << ": " << serializer.lastError().toStdString();

        EXPECT_FALSE(pm.loadProject(projectPath)) << c.label;
        EXPECT_EQ(pm.currentProject(), nullptr) << c.label;
        EXPECT_EQ(projectFileBytes(projectRoot), before) << c.label;
    }
}
