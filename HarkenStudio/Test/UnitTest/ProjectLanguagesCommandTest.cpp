#include "ActionManager.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>

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

TEST_F(ProjectLanguagesCommandTest, AcceptsEmptyList)
{
    const auto r = setLanguages({});
    ASSERT_TRUE(r.success);

    EXPECT_TRUE(languages().isEmpty());
    EXPECT_EQ(stack()->count(), 1);

    stack()->undo();
    EXPECT_EQ(languages(),
              (QStringList{QStringLiteral("English"), QStringLiteral("Japanese")}));
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
    ASSERT_EQ(pm.getDefaultLanguage(), QStringLiteral("English"));

    ASSERT_TRUE(setLanguages({QStringLiteral("Japanese")}).success);

    EXPECT_EQ(languages(), (QStringList{QStringLiteral("Japanese")}));
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
