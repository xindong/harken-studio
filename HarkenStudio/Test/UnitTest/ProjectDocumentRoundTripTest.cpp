#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/BindingSemantics.h"
#include "Core/EffectChainSemantics.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QScopeGuard>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <utility>

namespace {

using namespace VNX;

QByteArray readFile(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

QJsonObject readObject(const QString& path)
{
    const QJsonDocument document = QJsonDocument::fromJson(readFile(path));
    EXPECT_TRUE(document.isObject()) << path.toStdString();
    return document.object();
}

void writeObject(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    ASSERT_EQ(file.write(bytes), bytes.size());
}

QMap<QString, QString> documentPathsById(const QString& projectDirectory)
{
    QMap<QString, QString> paths;
    QDirIterator files(projectDirectory,
                       QStringList{QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                       QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        const QString id = HarkenTest::projectHeader(readObject(path))
                               .value(QStringLiteral("id")).toString();
        EXPECT_FALSE(id.isEmpty()) << path.toStdString();
        EXPECT_FALSE(paths.contains(id)) << id.toStdString();
        paths.insert(id, path);
    }
    return paths;
}

QJsonObject extensionMetadata(const QString& id)
{
    return {
        {QStringLiteral("futureAuthorNotes"), QJsonObject{
            {QStringLiteral("blob"), QString(4096, QLatin1Char('a'))},
            {QStringLiteral("document"), id},
            {QStringLiteral("values"), QJsonArray{
                QJsonValue::Null, true, QJsonValue(qint64(9007199254740993LL)),
                -2.5, QStringLiteral("Unedited metadata"), QJsonObject{}, QJsonArray{},
            }},
        }},
    };
}

ProjectItem* findItem(ProjectData& project, const QUuid& guid)
{
    const auto find = [&](auto&& self, ProjectItem* item) -> ProjectItem* {
        if (!item) return nullptr;
        if (item->guid == guid) return item;
        for (const auto& child : item->children)
            if (ProjectItem* found = self(self, child.get())) return found;
        return nullptr;
    };
    for (ProjectFileBinding* binding : project.allFileBindings())
        if (ProjectItem* item = find(find, binding->rootItem.get())) return item;
    return nullptr;
}

TEST(ProjectDocumentModuleData, KnownBindingAndEffectEditsPreserveRawModuleFields)
{
    const QJsonObject futureData = extensionMetadata(QStringLiteral("module"));
    ParameterBindingDef binding = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), QUuid::createUuid());
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(41));
    binding.address.path = BindingPath{3};
    QJsonObject bindingJson = binding.toJson().toObject();
    QJsonObject address = bindingJson.value(QStringLiteral("address")).toObject();
    address.insert(QStringLiteral("futureAddressData"), futureData);
    bindingJson.insert(QStringLiteral("address"), address);
    ParameterBindingDef loadedBinding = ParameterBindingDef::fromJson(bindingJson);
    ASSERT_TRUE(loadedBinding.typed);
    loadedBinding.outputMax = 12.0f;
    loadedBinding.address.path.reset();
    const QJsonObject savedBinding = loadedBinding.toJson().toObject();
    const QJsonObject savedAddress = savedBinding.value(QStringLiteral("address")).toObject();
    EXPECT_EQ(savedAddress.value(QStringLiteral("futureAddressData")), futureData);
    const ParameterBindingDef reopenedBinding = ParameterBindingDef::fromJson(savedBinding);
    ASSERT_TRUE(reopenedBinding.typed);
    EXPECT_EQ(reopenedBinding.addressExtensions.value(QStringLiteral("futureAddressData")),
              futureData);

    const QJsonObject effectJson{
        {QStringLiteral("runtimeStableId"), 42},
        {QStringLiteral("type"), QStringLiteral("gain")},
        {QStringLiteral("parameters"), QJsonArray{-3.0}},
        {QStringLiteral("bypass"), true},
        {QStringLiteral("futureEffectData"), futureData},
    };
    EffectEntry effect = EffectEntry::fromJson(effectJson);
    ASSERT_TRUE(effect.typed);
    effect.parameters[0] = -6.0f;
    effect.bypass = false;
    const QJsonObject savedEffect = effect.toJson().toObject();
    EXPECT_EQ(savedEffect.value(QStringLiteral("futureEffectData")), futureData);
    const EffectEntry reopenedEffect = EffectEntry::fromJson(savedEffect);
    ASSERT_TRUE(reopenedEffect.typed);
    EXPECT_EQ(reopenedEffect.extensions.value(QStringLiteral("futureEffectData")),
              futureData);
}

class ProjectDocumentRoundTripTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char executable[] = "harken_editor_tests";
            static char* argv[] = {executable, nullptr};
            static QApplication app(argc, argv);
        }
        ASSERT_TRUE(directory_.isValid());
        auto& manager = ProjectManager::instance();
        ProjectConfig config;
        config.name = QStringLiteral("MetadataRoundTrip");
        config.location = directory_.path();
        ASSERT_TRUE(manager.createProject(config));
        auto* layer = manager.createItem(
            nullptr, QStringLiteral("Main"), ProjectItem::Type::Layer);
        ASSERT_NE(layer, nullptr);
        auto* childLayer = manager.createItem(
            layer, QStringLiteral("Nested"), ProjectItem::Type::Layer);
        ASSERT_NE(childLayer, nullptr);
        ASSERT_NE(manager.createItem(childLayer, QStringLiteral("Tone"),
                                     ProjectItem::Type::Sound), nullptr);
        for (const auto& [name, type] : {
                 std::pair{QStringLiteral("Trigger"), ProjectItem::Type::Action},
                 std::pair{QStringLiteral("Space"), ProjectItem::Type::Attenuation},
                 std::pair{QStringLiteral("Intensity"), ProjectItem::Type::Variable},
             }) {
            ASSERT_NE(manager.createItem(nullptr, name, type), nullptr);
        }
        ASSERT_NE(manager.createItem(manager.getMasterBusItem(),
                                     QStringLiteral("Music"),
                                     ProjectItem::Type::Bus), nullptr);
        ASSERT_TRUE(manager.saveProject());
        projectPath_ = manager.currentProject()->projectPath;
        projectDirectory_ = QFileInfo(projectPath_).absolutePath();
        ASSERT_TRUE(manager.closeProject());

        QDirIterator documents(projectDirectory_,
                                {QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                                QDir::Files, QDirIterator::Subdirectories);
        while (documents.hasNext()) {
            const QString path = documents.next();
            QJsonObject document = readObject(path);
            QJsonObject header = HarkenTest::projectHeader(document);
            const QString id = header.value(QStringLiteral("id")).toString();
            const QJsonObject metadata = extensionMetadata(id);
            header.insert(QStringLiteral("futureHeaderMetadata"), metadata);
            document.insert(QStringLiteral("harkenDocument"), header);
            QJsonObject content = HarkenTest::projectContent(document);
            content.insert(QStringLiteral("extensions"), metadata);
            document.insert(QStringLiteral("content"), content);
            writeObject(path, document);
            documentsById_.insert(id, document);
            pathsById_.insert(id, path);
        }
        ASSERT_FALSE(documentsById_.isEmpty());
    }

    void TearDown() override
    {
        ProjectManager::instance().closeProject();
    }

    void expectModelMetadata(ProjectData& project)
    {
        for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
            const QJsonObject expectedHeader = HarkenTest::projectHeader(it.value());
            QJsonObject header;
            QJsonObject extensions;
            if (expectedHeader.value(QStringLiteral("type"))
                == QStringLiteral("project.root")) {
                header = project.documentHeader;
                extensions = project.extensions;
            } else {
                ProjectItem* item = findItem(project, QUuid::fromString(it.key()));
                ASSERT_NE(item, nullptr) << it.key().toStdString();
                header = item->documentHeader;
                extensions = item->extensions;
            }
            EXPECT_EQ(header.value(QStringLiteral("futureHeaderMetadata")),
                      expectedHeader.value(QStringLiteral("futureHeaderMetadata")));
            EXPECT_EQ(extensions, extensionMetadata(it.key()));
        }
    }

    QTemporaryDir directory_;
    QString projectPath_;
    QString projectDirectory_;
    QMap<QString, QJsonObject> documentsById_;
    QMap<QString, QString> pathsById_;
};

TEST_F(ProjectDocumentRoundTripTest, AllDocumentMetadataSurvivesEditingSavingAndReopening)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData& project = *manager.currentProject();
    expectModelMetadata(project);
    for (auto it = pathsById_.cbegin(); it != pathsById_.cend(); ++it)
        EXPECT_EQ(readObject(it.value()), documentsById_.value(it.key()));

    project.description = QStringLiteral("Edited by the baseline reader");
    for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
        const QJsonObject header = HarkenTest::projectHeader(it.value());
        if (header.value(QStringLiteral("type")) == QStringLiteral("project.root"))
            continue;
        ProjectItem* item = findItem(project, QUuid::fromString(it.key()));
        ASSERT_NE(item, nullptr);
        if (item == manager.getMasterBusItem()) continue;
        const QString name = QStringLiteral("Edited_") + item->name;
        ASSERT_TRUE(manager.renameItem(item, name));
    }
    ASSERT_TRUE(manager.saveProject());
    expectModelMetadata(project);
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData& reopened = *manager.currentProject();
    expectModelMetadata(reopened);
    const auto savedPaths = documentPathsById(projectDirectory_);
    EXPECT_EQ(savedPaths.keys(), documentsById_.keys());
    for (auto it = savedPaths.cbegin(); it != savedPaths.cend(); ++it) {
        const QJsonObject document = readObject(it.value());
        const QJsonObject header = HarkenTest::projectHeader(document);
        EXPECT_EQ(header.value(QStringLiteral("futureHeaderMetadata")),
                  HarkenTest::projectHeader(documentsById_.value(it.key()))
                      .value(QStringLiteral("futureHeaderMetadata")));
        EXPECT_EQ(HarkenTest::projectContent(document).value(QStringLiteral("extensions")),
                  extensionMetadata(it.key()));
    }
}

#ifdef Q_OS_WIN
TEST_F(ProjectDocumentRoundTripTest, OccupiedFilesDoNotDamageCommittedContentWhenSavingFails)
{
    const QDir root(projectDirectory_);
    const QString folderPath = root.filePath(QStringLiteral("Layers/Occupied"));
    const QString emptyPath = QDir(folderPath).filePath(QStringLiteral("Empty"));
    const QString ordinaryPath = QDir(folderPath).filePath(QStringLiteral("000_locked.bin"));
    ASSERT_TRUE(QDir().mkpath(emptyPath));
    QFile ordinary(ordinaryPath);
    ASSERT_TRUE(ordinary.open(QIODevice::WriteOnly));
    ASSERT_EQ(ordinary.write("retained bytes"), 14);
    ordinary.close();

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    auto* sound = manager.createItem(nullptr, QStringLiteral("OccupiedSound"),
        ProjectItem::Type::Sound, QStringLiteral("Occupied"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(manager.saveProject());
    const HANDLE occupied = CreateFileW(reinterpret_cast<LPCWSTR>(ordinaryPath.utf16()),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(occupied, INVALID_HANDLE_VALUE);
    auto release = qScopeGuard([&] { CloseHandle(occupied); });
    QMap<QString, QByteArray> committed;
    QDirIterator files(projectDirectory_, {QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
        QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        committed.insert(path, readFile(path));
    }
    auto* folder = manager.currentProject()->findFolderByPath(
        QStringLiteral("Layers"), QStringLiteral("Occupied"));
    ASSERT_NE(folder, nullptr);
    ASSERT_TRUE(manager.deleteEntries({}, {folder}));
    manager.currentProject()->description = QStringLiteral("Pending failed save");
    EXPECT_FALSE(manager.saveProject());
    for (auto it = committed.cbegin(); it != committed.cend(); ++it)
        EXPECT_EQ(readFile(it.key()), it.value()) << it.key().toStdString();
    EXPECT_EQ(readFile(ordinaryPath), QByteArray("retained bytes"));
    EXPECT_TRUE(QDir(emptyPath).exists());
}
#endif

}
