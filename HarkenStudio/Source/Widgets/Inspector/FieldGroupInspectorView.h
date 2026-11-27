#pragma once

#include "Core/PlatformContext.h"
#include "Core/FieldGroupEditing.h"
#include "Core/FieldGroupProjectAdapter.h"

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
    bool uniform = false;
    QVariant value;
    QVector<QPair<FieldGroupScope, QVariant>> perScope;
};

struct FieldGroupInspectorView {
    QUuid definition;
    FieldGroupId group = FieldGroupId::Output;
    PlatformContext context;
    DefinitionFieldGroups expectedBefore;
    FieldGroupEditScopes editScopes;
    QVector<QUuid> ancestry;
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
    std::optional<EffectChainLedger> effectChain() const;
    QJsonObject actionArgs() const;
    QString sourceTooltip(const ProjectData& project, const QString& fieldName = {}) const;
};

class FieldGroupInspectorRead {
public:
    explicit FieldGroupInspectorRead(const ProjectData& project);
    FieldGroupInspectorView read(const QUuid& definition, FieldGroupId group,
                                 const PlatformContext& context);
    QString sourceTooltip(const FieldGroupInspectorView& view, const QString& fieldName = {}) const;

private:
    const ProjectData& project_;
    std::shared_ptr<const bridge::CanonicalRuntimeProjectSourceIndexV1> source_;
    FieldGroupProjectView view_;
    FieldGroupResolver resolver_;
    std::map<FieldGroupScope, QStringList> scopeDiagnostics_;
};

FieldGroupInspectorView buildFieldGroupInspectorView(
    const ProjectData& project, const QUuid& definition,
    FieldGroupId group, const PlatformContext& context);

}
