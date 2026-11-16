#pragma once

#include "ProjectSchemaRules.h"

#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace VNX::GeneratedSchemaRegistry {

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
                          VNX::ProjectSchemaRuleExecutor& executor,
                          QStringList& errors);

}
