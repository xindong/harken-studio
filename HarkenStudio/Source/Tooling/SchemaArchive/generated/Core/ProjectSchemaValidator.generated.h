#pragma once

#include "ProjectSchemaRules.h"

#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace harken::studio::GeneratedSchemaRegistry {

bool supportsProjectVersion(int version);
bool supportsPackVersion(int version);
bool validateProjectDocument(int version, QStringView documentType,
                             const QJsonValue& document,
                             const QString& relativePath,
                             QStringList& errors);
bool validatePackMetadata(int version,
                          const QJsonValue& document,
                          const QString& relativePath,
                          QStringList& errors);
bool validateProjectRules(int version,
                          harken::studio::ProjectSchemaRuleExecutor& executor,
                          QStringList& errors);
bool validateDevelopmentProjectV9Document(QStringView documentType,
                                          const QJsonValue& document,
                                          const QString& relativePath,
                                          QStringList& errors);
void validateDevelopmentProjectV9Rules(harken::studio::ProjectSchemaRuleExecutor& executor);

}
