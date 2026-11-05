#pragma once

#include <QString>

class QAction;
class QObject;

namespace VNX {

class UndoRedoManager;

QAction* createUndoAction(UndoRedoManager& manager, QObject* parent, const QString& prefix = {});
QAction* createRedoAction(UndoRedoManager& manager, QObject* parent, const QString& prefix = {});

}
