#include "UnitTest/StudioObjectIdTestSupport.h"
#include <harken_project_schema/FrontEnd.h>
#include <harken/core/CurrentWorldArtifact.h>
#include <harken/core/CurrentWorldCodec.h>
#include <harken/param/VariableService.h>

#include "CurrentWorldProjectAdapter.h"
#include "CurrentWorldArtifactWriter.h"
#include "CurrentWorldArtifactWriterInternal.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "Components/VAction.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>
#include <utility>

namespace VNX {
namespace {

std::shared_ptr<ProjectItem> makeItem(const QString& name,
                                      ProjectItem::Type type,
                                      uint32_t objectId,
                                      harkenstudio::ObjectId stableId)
{
    auto item = std::make_shared<ProjectItem>(name, type);
    item->objectID = objectId;
    item->guid = VNX::authoringUuid(harkentest::StudioObjectId(stableId));
    item->attachDefaultComponents();
    return item;
}

void appendChild(const std::shared_ptr<ProjectItem>& parent,
                 const std::shared_ptr<ProjectItem>& child)
{
    child->parent = parent.get();
    parent->children.push_back(child);
}

ProjectData makeRandomProject(std::shared_ptr<ProjectItem>* outContainer,
                              std::shared_ptr<ProjectItem>* outFirstSound)
{
    ProjectData project = HarkenTest::projectWithPlatforms(
        {QStringLiteral("Windows"), QStringLiteral("Android")});
    project.nextObjectID = 6;

    auto master = makeItem(QStringLiteral("Master"), ProjectItem::Type::Bus,
                           5, harkentest::StudioObjectId(0x4d4153544552));
    ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    auto layer = makeItem(QStringLiteral("Layer"), ProjectItem::Type::Layer,
                          1, harkentest::StudioObjectId(2));
    layer->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    auto container = makeItem(QStringLiteral("Random"),
                              ProjectItem::Type::Container, 2, harkentest::StudioObjectId(3));
    auto first = makeItem(QStringLiteral("First"), ProjectItem::Type::Sound,
                          3, harkentest::StudioObjectId(4));
    auto second = makeItem(QStringLiteral("Second"), ProjectItem::Type::Sound,
                           4, harkentest::StudioObjectId(5));
    appendChild(layer, container);
    appendChild(container, first);
    appendChild(container, second);
    HarkenTest::setSharedSoundSource(*first, 101);
    HarkenTest::setSharedSoundSource(*second, 102);
    EXPECT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, first->guid));
    EXPECT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, second->guid));

    auto* selection = container->comp<VNX::VSelection>();
    EXPECT_NE(selection, nullptr);
    selection->mode = SelectionMode::Random;
    selection->behavior.discipline = RandomDiscipline::Draw;
    selection->behavior.avoidRepeat = 0;
    selection->selections.shared.find(0)->weight = 0;
    selection->selections.shared.find(1)->weight = 3;

    ProjectFileBinding binding;
    binding.rootItem = layer;
    project.layers.push_back(std::move(binding));
    if (outContainer != nullptr) *outContainer = container;
    if (outFirstSound != nullptr) *outFirstSound = first;
    return project;
}

enum class ArtifactFailureStage { Write, Commit };

ArtifactFailureStage artifactFailureStage = ArtifactFailureStage::Write;

class FailingArtifactFile final : public CurrentWorldArtifactFile
{
public:
    explicit FailingArtifactFile(const QString& targetPath)
        : file_(targetPath)
    {
    }

    void setDirectWriteFallback(bool enabled) override
    {
        file_.setDirectWriteFallback(enabled);
    }

    bool open() override
    {
        return file_.open(QIODevice::WriteOnly);
    }

    qint64 write(const char* data, qint64 byteCount) override
    {
        if (artifactFailureStage == ArtifactFailureStage::Write)
            return -1;
        return file_.write(data, byteCount);
    }

    bool commit() override
    {
        if (artifactFailureStage == ArtifactFailureStage::Commit) {
            file_.cancelWriting();
            return false;
        }
        return file_.commit();
    }

private:
    QSaveFile file_;
};

std::unique_ptr<CurrentWorldArtifactFile> makeFailingArtifactFile(
    const QString& targetPath)
{
    return std::make_unique<FailingArtifactFile>(targetPath);
}

struct ArtifactFileFactoryReset
{
    ~ArtifactFileFactoryReset()
    {
        setCurrentWorldArtifactFileFactoryForTesting(nullptr);
    }
};

}

TEST(CurrentWorldProjectAdapter, BusIdentityIndexIgnoresHandleValueAndSiblingOrder)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    auto master = project.busses.front().rootItem;
    ASSERT_NE(master, nullptr);

    constexpr std::uint32_t kMatchingHandle = 71;
    constexpr std::uint32_t kDistinctHandle = 72;
    constexpr harkenstudio::ObjectId kMatchingId{0, kMatchingHandle};
    constexpr harkenstudio::ObjectId kDistinctId{
        UINT64_C(0x3da91a64b9474560), UINT64_C(0x100000048)};
    auto matching = makeItem(QStringLiteral("Matching"), ProjectItem::Type::Bus,
                             kMatchingHandle, kMatchingId);
    auto distinct = makeItem(QStringLiteral("Distinct"), ProjectItem::Type::Bus,
                             kDistinctHandle, kDistinctId);
    ASSERT_EQ(matching->runtimeId(), kMatchingId);
    ASSERT_EQ(distinct->runtimeId(), kDistinctId);
    ASSERT_NE(kDistinctId, harkenstudio::ObjectId(0, kDistinctHandle));
    appendChild(master, matching);
    appendChild(master, distinct);

    const auto verify = [&](std::uint32_t handle,
                            harkenstudio::ObjectId expectedId) {
        QString error;
        const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
        ASSERT_NE(request, nullptr) << error.toStdString();
        ASSERT_NE(request->identityIndex, nullptr);
        const auto* byHandle = request->identityIndex->findBySourceHandle(
            harkenprojectschema::RuntimeIdentityKindV1::Bus, handle);
        const auto* byStableId = request->identityIndex->find(expectedId);
        ASSERT_NE(byHandle, nullptr);
        ASSERT_NE(byStableId, nullptr);
        EXPECT_EQ(byHandle, byStableId);
        EXPECT_EQ(byHandle->stableId, expectedId);
        EXPECT_EQ(byHandle->authoringGuid, expectedId);
        ASSERT_NE(request->busResult, nullptr);
        EXPECT_NE(std::find(request->busResult->idSet.ids.begin(),
                            request->busResult->idSet.ids.end(), expectedId),
                  request->busResult->idSet.ids.end());
    };
    verify(kMatchingHandle, kMatchingId);
    verify(kDistinctHandle, kDistinctId);

    std::swap(master->children[0], master->children[1]);
    verify(kMatchingHandle, kMatchingId);
    verify(kDistinctHandle, kDistinctId);
}

TEST(CurrentWorldArtifactWriter,
     WriteAndCommitFailuresPreserveExistingTargetWithoutDirectFallback)
{
    harkenprojectschema::ProjectInput input;
    const auto frontEnd = harkenprojectschema::CompileFrontEnd(input);
    ASSERT_TRUE(frontEnd.ok());
    ASSERT_TRUE(frontEnd.project.has_value());
    auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        *frontEnd.project, {}, {}, {});
    ASSERT_TRUE(compiled.ok());
    ASSERT_NE(compiled.world, nullptr);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("existing.vnworld"));
    const QByteArray original("existing-current-world");
    QFile initial(path);
    ASSERT_TRUE(initial.open(QIODevice::WriteOnly));
    ASSERT_EQ(initial.write(original), original.size());
    initial.close();

    setCurrentWorldArtifactFileFactoryForTesting(&makeFailingArtifactFile);
    ArtifactFileFactoryReset reset;
    for (const ArtifactFailureStage stage : {
             ArtifactFailureStage::Write,
             ArtifactFailureStage::Commit}) {
        artifactFailureStage = stage;
        QString error;
        EXPECT_FALSE(WriteCurrentWorldArtifactV1(
            path, compiled.world->View(), &error));
        QFile preserved(path);
        ASSERT_TRUE(preserved.open(QIODevice::ReadOnly));
        EXPECT_EQ(preserved.readAll(), original);
    }
}

TEST(CurrentWorldArtifactWriter,
     CompilesStudioProjectIntoDecodableActionArtifact)
{
    ProjectData project = HarkenTest::projectWithPlatforms(
        {QStringLiteral("Windows"), QStringLiteral("Android")});
    project.nextObjectID = 5;
    auto master = makeItem(QStringLiteral("Master"), ProjectItem::Type::Bus,
                           4, harkentest::StudioObjectId(0x4d4153544552));
    ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));
    auto layer = makeItem(QStringLiteral("Layer"), ProjectItem::Type::Layer,
                          1, harkentest::StudioObjectId(2));
    layer->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    auto variable = makeItem(QStringLiteral("State"),
                             ProjectItem::Type::Variable, 2, harkentest::StudioObjectId(3));
    appendChild(layer, variable);
    auto action = makeItem(QStringLiteral("SetState"),
                           ProjectItem::Type::Action, 3, harkentest::StudioObjectId(4));
    VActionStep setValue;
    setValue.type = VActionStep::Type::SetVariable;
    setValue.targetObjectID = variable->objectID;
    setValue.value = 42.0f;
    action->comp<VAction>()->steps.push_back(setValue);
    appendChild(layer, action);
    ProjectFileBinding binding;
    binding.rootItem = layer;
    project.layers.push_back(std::move(binding));

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString requestedPath = qEnvironmentVariable(
        "HARKEN_TEST_CURRENT_WORLD_ARTIFACT_OUTPUT");
    const QString path = requestedPath.isEmpty()
        ? directory.filePath(QStringLiteral("project.vnworld"))
        : requestedPath;
    QString error;
    ASSERT_TRUE(WriteCurrentWorldArtifactV1(project, std::nullopt, path, &error))
        << error.toStdString();

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    harken::CurrentWorldArtifactHeaderV1 header;
    ASSERT_EQ(harken::InspectCurrentWorldArtifactHeaderV1(
                  reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                  static_cast<std::size_t>(bytes.size()),
                  static_cast<std::uint64_t>(bytes.size()), header),
              harken::CurrentWorldLoadStatusV1::Ok);
    harken::CurrentWorldBodyImage image;
    ASSERT_EQ(harken::DecodeCurrentWorldArtifactBodyV1(
                  header,
                  reinterpret_cast<const std::uint8_t*>(bytes.constData())
                      + harken::kCurrentWorldArtifactHeaderBytes,
                  static_cast<std::size_t>(bytes.size())
                      - harken::kCurrentWorldArtifactHeaderBytes,
                  image),
              harken::CurrentWorldLoadStatusV1::Ok);
    const auto& world = *image.ProductionView();
    ASSERT_EQ(world.variableCount, 1u);
    EXPECT_EQ(world.variables[0].variableId, variable->runtimeId());
    ASSERT_EQ(world.actionCount, 1u);
    ASSERT_EQ(world.actions[0].stepCount, 1u);
}

}
