#pragma once

#include "Core/PlatformContext.h"
#include "Core/FieldGroupEditing.h"

#include <QJsonObject>
#include <QPair>
#include <QStringList>
#include <QUuid>
#include <QVariant>
#include <QVector>

#include <vector>

namespace VNX {

struct ProjectData;

struct FieldGroupInspectorScopeView {
    FieldGroupScope scope;
    ResolvedFieldGroup resolved;
};

struct FieldGroupInspectorFieldView {
    bool uniform = false;
    QVariant value;
    QVector<QPair<FieldGroupScope, QVariant>> perScope;
};

struct FieldGroupInspectorView {
    QUuid definition;
    FieldGroupId group = FieldGroupId::MainOutput;
    PlatformContext context;
    DefinitionFieldGroups expectedBefore;
    FieldGroupEditScopes editScopes;
    std::vector<FieldGroupInspectorScopeView> perScope;
    QStringList diagnostics;
    bool valid = false;
    bool canInherit = false;
    bool fieldsEditable = false;
    bool effectiveInherit = true;
    bool inheritanceMixed = false;
    bool modesMixed = false;
    bool allExplicitLocal = false;
    bool anyUseShared = false;
    bool allUseShared = false;
    bool sendsUniform = false;

    FieldGroupInspectorFieldView field(const QString& fieldName) const;
    QJsonObject actionArgs() const;
    QString sourceTooltip(const ProjectData& project, const QString& fieldName = {}) const;
};

FieldGroupInspectorView buildFieldGroupInspectorView(
    const ProjectData& project, const QUuid& definition,
    FieldGroupId group, const PlatformContext& context);

}
