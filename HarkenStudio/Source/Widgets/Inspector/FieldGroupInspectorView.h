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

namespace harken::studio {

struct ProjectData;

struct FieldGroupInspectorScopeView {
    FieldGroupScope scope;
    ResolvedFieldGroup resolved;
};

struct FieldGroupInspectorFieldView {
    bool valid = false;
    QVariant value;
    QVariant shared;
    QVector<QPair<FieldGroupScope, QVariant>> perScope;
};

struct FieldGroupInspectorView {
    QUuid definition;
    FieldGroupId group = FieldGroupId::Output;
    PlatformContext context;
    DefinitionFieldGroups expectedBefore;
    FieldGroupEditScopes editScopes;
    std::vector<FieldGroupInspectorScopeView> perScope;
    std::optional<ResolvedFieldGroup> shared;
    QStringList diagnostics;
    bool valid = false;
    bool canInherit = false;
    bool fieldsEditable = false;
    bool effectiveInherit = true;
    bool inheritanceMixed = false;

    const FieldGroupInspectorScopeView* displayed() const;
    FieldGroupInspectorFieldView field(const QString& fieldName) const;
    std::optional<EffectChainLedger> effectChain() const;
    QJsonObject actionArgs(bool sharedScope = false) const;
    QString sourceTooltip(const ProjectData& project, const QString& fieldName = {}) const;
};

FieldGroupInspectorView buildFieldGroupInspectorView(
    const ProjectData& project, const QUuid& definition,
    FieldGroupId group, const PlatformContext& context);

}
