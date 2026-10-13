#pragma once

#include "BindingSemantics.h"

#include <QJsonObject>
#include <QString>

class QObject;

namespace VNX {

struct ProjectItem;
class VMixNode;

namespace BindingEditing {

VMixNode* ledgerHost(ProjectItem* item, BindingHostClass hostClass);
const VMixNode* ledgerHost(const ProjectItem* item, BindingHostClass hostClass);
VMixNode* ledgerHost(ProjectItem* item, const BindingAddress& address);
const VMixNode* ledgerHost(const ProjectItem* item, const BindingAddress& address);
BindingHostClass hostClassOf(const ProjectItem* item);

QString changeKey(const BindingAddress& address);

QJsonObject upsertArgs(const ProjectItem* item, const ParameterBindingDef& binding,
                       const QString& text);
QJsonObject removeArgs(const ProjectItem* item, harkenstudio::ObjectId runtimeStableId,
                       const QString& text);
QJsonObject removeAllArgs(const ProjectItem* item, const BindingAddress& address,
                          const QString& text);
QJsonObject reorderArgs(const ProjectItem* item, const BindingAddress& address,
                        int fromLocal, int toLocal, const QString& text);

bool apply(ProjectItem* item, const BindingLedger& ledger,
           const QObject* originator = nullptr);
bool apply(ProjectItem* item, const BindingLedger& ledger,
           const BindingAddress& address, const QObject* originator = nullptr);
bool upsert(ProjectItem* item, ParameterBindingDef binding,
            const QObject* originator = nullptr);
bool remove(ProjectItem* item, harkenstudio::ObjectId runtimeStableId,
            const QObject* originator = nullptr);
bool removeAll(ProjectItem* item, const BindingAddress& address,
               const QObject* originator = nullptr);
bool reorder(ProjectItem* item, const BindingAddress& address,
             int fromLocal, int toLocal, const QObject* originator = nullptr);

}

}
