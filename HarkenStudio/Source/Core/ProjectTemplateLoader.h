
#pragma once

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QVector>
#include <optional>
#include <vector>

namespace VNX {


struct TemplateBusNode {
    QString name;
    QJsonObject authoring;
    std::vector<TemplateBusNode> children;
};

struct TemplateAttenuationDef {
    QString name;
    double  maxDistance = 0.0;
    QString volumeCurve;
    QVector<QPointF> volumePoints;
    QJsonObject authoring;
};

struct ProjectTemplateData {
    static constexpr int kLegacyFormatVersion = 1;
    static constexpr int kCurrentFormatVersion = 2;

    int templateFormatVersion = kCurrentFormatVersion;
    QString id;
    QString displayName;
    QString description;
    TemplateBusNode bus;
    std::vector<TemplateAttenuationDef>  attenuations;
    QJsonArray content;
    QJsonArray variables;
    QJsonArray actions;

    QJsonObject toJson() const;
};

struct TemplateLocation {
    QString id;
    QString displayName;
    QString description;
    QString filePath;
};


struct ValidationError {
    QString jsonPath;
    QString message;
};

struct ValidationResult {
    bool valid = false;
    QString summary;
    std::vector<ValidationError> errors;

    QString toUserText() const;
};


class ProjectTemplateLoader {
public:
    static std::vector<TemplateLocation> listAvailable();

    static std::optional<ProjectTemplateData> load(const QString& id);

    static std::optional<ProjectTemplateData> loadFile(
        const QString& path, ValidationResult* result = nullptr);

    static ValidationResult validate(const QJsonDocument& doc,
                                     const QString& sourceLabel = QString());

    static std::optional<ProjectTemplateData> parse(
        const QJsonObject& object,
        ValidationResult* result = nullptr,
        const QString& id = QString());

    static std::optional<QString> importFromFile(const QString& srcPath,
                                                 ValidationResult& outResult);

    static QString runtimeDir();

    static void ensureUserTemplatesSeeded();
};

}
