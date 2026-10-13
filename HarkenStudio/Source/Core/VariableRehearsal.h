
#pragma once

#include <QHash>
#include <QObject>
#include <QUuid>

#include <optional>
#include "VariableSemantics.h"

namespace VNX {

// Session-level Variable rehearsal overrides (AuditionOverlay family): tool
// state that never dirties the document, never enters undo, never reaches
// disk. Values store raw; consumers clamp at read time so declaration edits
// (range, kind) re-clamp naturally.
class VariableRehearsal : public QObject
{
    Q_OBJECT

public:
    explicit VariableRehearsal(QObject* parent = nullptr);

    std::optional<float> overrideFor(const QUuid& variableId) const;

    bool setOverride(const QUuid& variableId, float value);
    VariableScope scopeFor(const QUuid& variableId) const;
    void setScope(const QUuid& variableId, VariableScope scope);
    void clearAll();

signals:
    void rehearsalChanged(const QUuid& variableId);
    void scopeChanged(const QUuid& variableId);

private:
    QHash<QUuid, float> overrides_;
    QHash<QUuid, VariableScope> scopes_;
};

}
