#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ProjectManager.h"
#include "ProjectSerializer.h"
#include "ProjectTypes.h"
#include "PlayablePropertyAuthoringV1.h"
#include "Components/VSelection.h"
#include "TransitionSemantics.h"
#include "Support/ProjectDocumentTestUtils.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTemporaryDir>
#include <QUuid>

#include <functional>

using VNX::ProjectData;
using VNX::ProjectItem;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
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

QJsonObject canonicalPlayableProperties()
{
    return QJsonObject{{QStringLiteral("presence"),
                        QStringLiteral("0000000000000000")}};
}

}

TEST(ProjectSerialization, NewProjectIdentityRemainsStableAcrossSaves)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    ProjectData project = HarkenTest::projectWithPlatforms();
    project.name = QStringLiteral("Identity");
    project.projectPath = QDir(tempDir.path()).filePath(QStringLiteral("Identity.hsp"));
    auto master = std::make_shared<ProjectItem>(QStringLiteral("Master"), ProjectItem::Type::Bus);
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

TEST(ProjectSerialization, DuplicateTransitionEndpointsRejectItemLoad)
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
    ProjectItem original;
    ASSERT_TRUE(serializer.projectItemFromJson(saved, original));
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

TEST(ProjectSerialization, NestedTreeDeserializationRestoresParentLinks)
{
    ProjectItem layer(QStringLiteral("Amb_Field"), ProjectItem::Type::Layer);
    layer.attachDefaultComponents();
    layer.guid = QUuid::createUuid();

    addChild(layer, "Amb_Rain", ProjectItem::Type::Sound);
    auto birds = addChild(layer, "Birds", ProjectItem::Type::Container);
    addChild(*birds, "Chirp_A", ProjectItem::Type::Sound);
    auto seq   = addChild(*birds, "Seq", ProjectItem::Type::Track);
    auto intro = addChild(*seq, "Intro", ProjectItem::Type::Piece);
    auto tail  = addChild(*intro, "Tail", ProjectItem::Type::Track);
    addChild(*tail, "Tail_Hit", ProjectItem::Type::Sound);

    VNX::ProjectSerializer serializer;

    std::function<QJsonObject(const ProjectItem&)> inlineWindowForm =
        [&](const ProjectItem& node) {
            QJsonObject json = serializer.audioItemToJson(node);
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
    const auto& birds2 = reloaded.children[1];
    EXPECT_EQ(birds2->parent, &reloaded);

    ASSERT_EQ(birds2->children.size(), 2u);
    const auto& seq2 = birds2->children[1];

    ASSERT_EQ(seq2->children.size(), 1u);
    const auto& intro2 = seq2->children[0];
    EXPECT_EQ(intro2->parent, seq2.get());

    ASSERT_EQ(intro2->children.size(), 1u);
    const auto& tail2 = intro2->children[0];
    ASSERT_EQ(tail2->children.size(), 1u);
    EXPECT_EQ(tail2->children[0]->parent, tail2.get());
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
