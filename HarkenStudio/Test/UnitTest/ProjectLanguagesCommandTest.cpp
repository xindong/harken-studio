#include "ActionManager.h"
#include "LanguageContextService.h"
#include "MemorySettingsStorage.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class ProjectLanguagesCommandTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(),
                                    &VNX::ProjectManager::instance(), nullptr, {});

        ASSERT_TRUE(tempDir_.isValid());
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();

        VNX::ProjectConfig config;
        config.name = QStringLiteral("ProjectLanguagesProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(pm.createProject(config));

        pm.setProjectLanguagesInternal(
            QStringList{QStringLiteral("English"), QStringLiteral("Japanese")});
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ProjectManager::instance().closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    VNX::ActionResult setLanguages(const QStringList& languages)
    {
        QJsonArray arr;
        for (const auto& lang : languages) arr.append(lang);
        return VNX::ActionManager::instance().execute(
            QStringLiteral("set_project_languages"),
            QJsonObject{{"languages", arr}});
    }

    QStringList languages() const
    {
        return VNX::ProjectManager::instance().getProjectLanguages();
    }

    QUndoStack* stack() { return VNX::UndoRedoManager::instance().undoStack(); }

    QTemporaryDir tempDir_;
};

}

TEST_F(ProjectLanguagesCommandTest, SetReplacesWholeList)
{
    const auto r = setLanguages(
        {QStringLiteral("Korean"), QStringLiteral(" English "), QStringLiteral("")});
    ASSERT_TRUE(r.success);

    EXPECT_EQ(languages(),
              (QStringList{QStringLiteral("Korean"), QStringLiteral("English")}));

    QStringList echoed;
    for (const auto& v : r.data.value(QStringLiteral("languages")).toArray())
        echoed.append(v.toString());
    EXPECT_EQ(echoed, languages());
}

TEST_F(ProjectLanguagesCommandTest, UndoRedoRestoresExactly)
{
    const QStringList before = languages();

    ASSERT_TRUE(setLanguages({QStringLiteral("Korean")}).success);
    EXPECT_EQ(stack()->count(), 1);
    ASSERT_EQ(languages(), (QStringList{QStringLiteral("Korean")}));

    stack()->undo();
    EXPECT_EQ(languages(), before);

    stack()->redo();
    EXPECT_EQ(languages(), (QStringList{QStringLiteral("Korean")}));
}

TEST_F(ProjectLanguagesCommandTest, LoadedLegacyNamesSurviveUndoRedoAndReload)
{
    auto& pm = VNX::ProjectManager::instance();
    const QStringList legacy = {QStringLiteral("zh-CN"), QString(33, QLatin1Char('A'))};
    const QStringList replacement = {QStringLiteral("English")};
    const QString projectPath = pm.currentProject()->projectPath;
    ASSERT_TRUE(pm.closeProject());
    QFile file(projectPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    auto content = root.value(QStringLiteral("content")).toObject();
    content.insert(QStringLiteral("projectLanguages"), QJsonArray::fromStringList(legacy));
    root.insert(QStringLiteral("content"), content);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const auto bytes = QJsonDocument(root).toJson();
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();
    ASSERT_TRUE(pm.loadProject(projectPath));
    ASSERT_EQ(languages(), legacy);
    VNX::LanguageContextService context(std::make_unique<VNX::MemorySettingsStorage>());
    EXPECT_EQ(context.availableLanguages(), legacy);
    EXPECT_EQ(context.language(), legacy.front());
    EXPECT_FALSE(setLanguages(legacy).success);
    EXPECT_EQ(stack()->count(), 0);

    ASSERT_TRUE(setLanguages(replacement).success);
    EXPECT_EQ(languages(), replacement);
    EXPECT_EQ(context.language(), replacement.front());
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_EQ(languages(), legacy);
    EXPECT_EQ(context.availableLanguages(), legacy);
    EXPECT_EQ(context.language(), legacy.front());
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    EXPECT_EQ(languages(), replacement);
    EXPECT_EQ(context.availableLanguages(), replacement);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.closeProject());
    ASSERT_TRUE(pm.loadProject(projectPath));
    EXPECT_EQ(languages(), legacy);
    EXPECT_EQ(context.availableLanguages(), legacy);
    EXPECT_EQ(context.language(), legacy.front());
}

TEST_F(ProjectLanguagesCommandTest, RejectsDuplicateIgnoringCase)
{
    const QStringList before = languages();

    const auto r = setLanguages({QStringLiteral("English"), QStringLiteral("english")});
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorCode, QStringLiteral("INVALID_ARGS"));
    EXPECT_TRUE(r.errorMessage.contains(QStringLiteral("english")));
    EXPECT_EQ(languages(), before);
    EXPECT_EQ(stack()->count(), 0);
}

TEST_F(ProjectLanguagesCommandTest, RejectsSfx)
{
    const QStringList before = languages();

    const auto r = setLanguages({QStringLiteral("English"), QStringLiteral("SFX")});
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorCode, QStringLiteral("INVALID_ARGS"));
    EXPECT_TRUE(r.errorMessage.contains(QStringLiteral("SFX")));
    EXPECT_EQ(languages(), before);
    EXPECT_EQ(stack()->count(), 0);
}

TEST_F(ProjectLanguagesCommandTest, RejectsIllegalCharacters)
{
    const QStringList before = languages();

    const auto r = setLanguages({QStringLiteral("zh-CN")});
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorCode, QStringLiteral("INVALID_ARGS"));
    EXPECT_TRUE(r.errorMessage.contains(QStringLiteral("zh-CN")));
    EXPECT_EQ(languages(), before);
    EXPECT_EQ(stack()->count(), 0);
}

TEST_F(ProjectLanguagesCommandTest, RejectsOverlongName)
{
    const QStringList before = languages();

    const auto r = setLanguages({QString(33, QLatin1Char('A'))});
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorCode, QStringLiteral("INVALID_ARGS"));
    EXPECT_EQ(languages(), before);
    EXPECT_EQ(stack()->count(), 0);
}

TEST_F(ProjectLanguagesCommandTest, EmptyLanguagesSurviveUndoRedoAndReload)
{
    auto& pm = VNX::ProjectManager::instance();
    const auto before = languages();
    VNX::LanguageContextService context(std::make_unique<VNX::MemorySettingsStorage>());

    ASSERT_TRUE(setLanguages({}).success);
    EXPECT_TRUE(languages().isEmpty());
    EXPECT_TRUE(context.language().isEmpty());

    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_EQ(languages(), before);
    EXPECT_EQ(context.language(), before.front());

    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    EXPECT_TRUE(languages().isEmpty());
    EXPECT_TRUE(context.language().isEmpty());

    const QString projectPath = pm.currentProject()->projectPath;
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.closeProject());
    ASSERT_TRUE(pm.loadProject(projectPath));
    EXPECT_TRUE(languages().isEmpty());
    EXPECT_TRUE(context.availableLanguages().isEmpty());
    EXPECT_TRUE(context.language().isEmpty());
}

TEST_F(ProjectLanguagesCommandTest, SameValueIsNoOp)
{
    const auto r = setLanguages({QStringLiteral("English"), QStringLiteral("Japanese")});
    EXPECT_TRUE(r.success);
    EXPECT_EQ(stack()->count(), 0);
    EXPECT_EQ(languages(),
              (QStringList{QStringLiteral("English"), QStringLiteral("Japanese")}));
}

TEST_F(ProjectLanguagesCommandTest, DoesNotTouchDefaultLanguage)
{
    auto& pm = VNX::ProjectManager::instance();
    pm.setDefaultLanguage(QStringLiteral("English"));

    ASSERT_TRUE(setLanguages({QStringLiteral("Japanese")}).success);

    EXPECT_EQ(languages(), (QStringList{QStringLiteral("Japanese")}));
    EXPECT_EQ(pm.getDefaultLanguage(), QStringLiteral("English"));

    ASSERT_TRUE(setLanguages({}).success);
    EXPECT_EQ(pm.getDefaultLanguage(), QStringLiteral("English"));

    const QString projectPath = pm.currentProject()->projectPath;
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.closeProject());
    ASSERT_TRUE(pm.loadProject(projectPath));
    EXPECT_TRUE(languages().isEmpty());
    EXPECT_EQ(pm.getDefaultLanguage(), QStringLiteral("English"));
}

TEST_F(ProjectLanguagesCommandTest, ProjectBecomesModified)
{
    ASSERT_TRUE(VNX::UndoRedoManager::instance().isClean());

    ASSERT_TRUE(setLanguages({QStringLiteral("Korean")}).success);
    EXPECT_FALSE(VNX::UndoRedoManager::instance().isClean());

    stack()->undo();
    EXPECT_TRUE(VNX::UndoRedoManager::instance().isClean());
}

TEST_F(ProjectLanguagesCommandTest, SurvivesSaveAndReload)
{
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(setLanguages({QStringLiteral("Korean"), QStringLiteral("Japanese")}).success);

    const QString projectPath = pm.currentProject()->projectPath;
    QString saveError;
    const auto errorConn = QObject::connect(
        &pm, &VNX::ProjectManager::errorOccurred,
        [&saveError](const QString& error) { saveError = error; });
    ASSERT_TRUE(pm.saveProject()) << saveError.toStdString();
    QObject::disconnect(errorConn);

    VNX::UndoRedoManager::instance().clear();
    pm.closeProject();
    ASSERT_TRUE(pm.loadProject(projectPath));

    EXPECT_EQ(languages(),
              (QStringList{QStringLiteral("Korean"), QStringLiteral("Japanese")}));
}

TEST_F(ProjectLanguagesCommandTest, LegacyEmptyLanguagesRemainEmptyAfterSave)
{
    auto& pm = VNX::ProjectManager::instance();
    const QString projectPath = pm.currentProject()->projectPath;
    QFile file(projectPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto original = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    for (const QJsonValue& legacy : {QJsonValue(), QJsonValue(QJsonArray{}),
                                    QJsonValue(QJsonArray{QStringLiteral("SFX")})}) {
        pm.closeProject();
        auto root = original;
        auto content = root.value(QStringLiteral("content")).toObject();
        if (legacy.isNull()) content.remove(QStringLiteral("projectLanguages"));
        else content.insert(QStringLiteral("projectLanguages"), legacy);
        root.insert(QStringLiteral("content"), content);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(root).toJson());
        file.close();
        ASSERT_TRUE(pm.loadProject(projectPath));
        EXPECT_TRUE(languages().isEmpty());
        ASSERT_TRUE(pm.addProjectLanguage(QStringLiteral("English")));
        ASSERT_TRUE(pm.removeProjectLanguage(QStringLiteral("English")));
        EXPECT_TRUE(languages().isEmpty());
        ASSERT_TRUE(pm.saveProject());
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(file.readAll()).object().value("content").toObject();
        EXPECT_TRUE(saved.value("projectLanguages").toArray().isEmpty());
        file.close();
    }
}
