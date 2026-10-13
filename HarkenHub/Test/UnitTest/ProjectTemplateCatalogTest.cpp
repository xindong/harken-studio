#include "Templates/ProjectTemplateCatalog.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace VNH;

namespace {

QJsonObject validTemplate(const QString& name = QStringLiteral("Template"))
{
    return {
        {QStringLiteral("templateFormatVersion"), 2},
        {QStringLiteral("displayName"), name},
        {QStringLiteral("description"), QStringLiteral("Description")},
        {QStringLiteral("bus"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("Master Bus")},
                     {QStringLiteral("children"), QJsonArray{}}}},
        {QStringLiteral("attenuation"), QJsonArray{}}};
}

QString writeTemplate(const QString& directory, const QString& fileName,
                      const QJsonObject& object)
{
    const QString path = QDir(directory).filePath(fileName);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return path;
}

class ProjectTemplateCatalogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(root_.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", root_.path().toUtf8());
    }

    void TearDown() override { qunsetenv("HARKEN_HUB_USER_ROOT"); }

    QTemporaryDir root_;
};

}

TEST_F(ProjectTemplateCatalogTest, AcceptsCurrentTemplateFormat)
{
    const QString path = writeTemplate(root_.path(), QStringLiteral("valid.json"),
                                       validTemplate());
    const TemplateValidationResult result =
        ProjectTemplateCatalog::validateFile(path);
    EXPECT_TRUE(result.valid) << result.toUserText().toStdString();
}

TEST_F(ProjectTemplateCatalogTest, RejectsInvalidTemplateContent)
{
    QJsonObject object = validTemplate();
    object[QStringLiteral("content")] = QJsonArray{
        QJsonObject{{QStringLiteral("type"), 8}}};
    const QString path = writeTemplate(root_.path(), QStringLiteral("bad.json"),
                                       object);
    const TemplateValidationResult result =
        ProjectTemplateCatalog::validateFile(path);
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.issues.empty());
}

TEST_F(ProjectTemplateCatalogTest, RejectsUnsupportedTopLevelSection)
{
    QJsonObject object = validTemplate();
    object[QStringLiteral("legacyVariables")] = QJsonArray{};
    const QString path = writeTemplate(root_.path(), QStringLiteral("legacy.json"),
                                       object);
    const TemplateValidationResult result =
        ProjectTemplateCatalog::validateFile(path);
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.issues.size(), 1u);
    EXPECT_EQ(result.issues.front().path, QStringLiteral("legacyVariables"));
}

TEST_F(ProjectTemplateCatalogTest, RevalidatesTemplateAfterFileChanges)
{
    const QString path = writeTemplate(root_.path(), QStringLiteral("live.json"),
                                       validTemplate());
    ASSERT_TRUE(ProjectTemplateCatalog::loadFile(path, true));

    QJsonObject damaged = validTemplate();
    damaged.remove(QStringLiteral("bus"));
    ASSERT_FALSE(writeTemplate(root_.path(), QStringLiteral("live.json"),
                               damaged)
                     .isEmpty());

    TemplateValidationResult validation;
    EXPECT_FALSE(ProjectTemplateCatalog::loadFile(path, true, &validation));
    EXPECT_FALSE(validation.valid);
    EXPECT_FALSE(validation.issues.empty());
}

TEST_F(ProjectTemplateCatalogTest, ImportPersistsAndDeduplicatesTemplate)
{
    QTemporaryDir sourceDir;
    ASSERT_TRUE(sourceDir.isValid());
    const QString source = writeTemplate(sourceDir.path(),
                                         QStringLiteral("custom.json"),
                                         validTemplate(QStringLiteral("Custom")));
    TemplateValidationResult validation;
    const auto first =
        ProjectTemplateCatalog::importFromFile(source, &validation);
    ASSERT_TRUE(first) << validation.toUserText().toStdString();
    EXPECT_TRUE(QFileInfo::exists(first->filePath));

    const auto second =
        ProjectTemplateCatalog::importFromFile(source, &validation);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->filePath, first->filePath);
    EXPECT_EQ(QDir(ProjectTemplateCatalog::userTemplateDir())
                  .entryList({QStringLiteral("*.json")}, QDir::Files)
                  .size(),
              1);
}

TEST_F(ProjectTemplateCatalogTest, ListsValidBuiltInAndUserTemplates)
{
    QTemporaryDir builtInDir;
    ASSERT_TRUE(builtInDir.isValid());
    ASSERT_FALSE(writeTemplate(builtInDir.path(), QStringLiteral("builtin.json"),
                               validTemplate(QStringLiteral("Built-in")))
                     .isEmpty());
    ASSERT_TRUE(QDir().mkpath(ProjectTemplateCatalog::userTemplateDir()));
    ASSERT_FALSE(writeTemplate(ProjectTemplateCatalog::userTemplateDir(),
                               QStringLiteral("user.json"),
                               validTemplate(QStringLiteral("User")))
                     .isEmpty());
    ASSERT_FALSE(writeTemplate(ProjectTemplateCatalog::userTemplateDir(),
                               QStringLiteral("invalid.json"), QJsonObject{})
                     .isEmpty());

    const auto entries =
        ProjectTemplateCatalog::listAvailable(builtInDir.path());
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_FALSE(entries[0].userTemplate);
    EXPECT_TRUE(entries[1].userTemplate);
}
