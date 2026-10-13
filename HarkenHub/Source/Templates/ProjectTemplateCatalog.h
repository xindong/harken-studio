#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>
#include <vector>

namespace VNH {

struct TemplateValidationIssue
{
    QString path;
    QString message;
};

struct TemplateValidationResult
{
    bool valid = false;
    QString summary;
    std::vector<TemplateValidationIssue> issues;

    QString toUserText() const;
};

struct ProjectTemplateEntry
{
    QString id;
    QString displayName;
    QString description;
    QString filePath;
    QJsonObject content;
    bool userTemplate = false;
};

class ProjectTemplateCatalog
{
public:
    static QString userTemplateDir();
    static TemplateValidationResult validateFile(const QString& path);
    static std::optional<ProjectTemplateEntry> loadFile(
        const QString& path, bool userTemplate,
        TemplateValidationResult* result = nullptr);
    static std::vector<ProjectTemplateEntry> listAvailable(
        const QString& builtInTemplateDir);
    static std::optional<ProjectTemplateEntry> importFromFile(
        const QString& sourcePath, TemplateValidationResult* result);
};

}
