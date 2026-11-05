#include "UndoActions.h"
#include "UndoCommands.h"

#include <QAction>

namespace VNX {

namespace {

QAction* createReplayAction(UndoRedoManager& manager, QObject* parent, const QString& prefix, bool redo)
{
    auto* action = new QAction(parent);
    const auto update = [&manager, action, prefix, redo]() {
        const auto label = prefix.isEmpty() ? (redo ? UndoRedoManager::tr("Redo") : UndoRedoManager::tr("Undo")) : prefix;
        const auto text = redo ? manager.undoStack()->redoText() : manager.undoStack()->undoText();
        action->setText(text.isEmpty() ? label : label + QChar(' ') + text);
        action->setEnabled(redo ? manager.canRedo() : manager.canUndo());
    };
    QObject::connect(&manager, &UndoRedoManager::canUndoChanged, action, update);
    QObject::connect(&manager, &UndoRedoManager::canRedoChanged, action, update);
    QObject::connect(manager.undoStack(), &UndoStack::undoTextChanged, action, update);
    QObject::connect(manager.undoStack(), &UndoStack::redoTextChanged, action, update);
    QObject::connect(action, &QAction::triggered, &manager, [&manager, redo]() {
        if (redo) manager.redo();
        else manager.undo();
    });
    update();
    return action;
}

}

QAction* createUndoAction(UndoRedoManager& manager, QObject* parent, const QString& prefix)
{
    return createReplayAction(manager, parent, prefix, false);
}

QAction* createRedoAction(UndoRedoManager& manager, QObject* parent, const QString& prefix)
{
    return createReplayAction(manager, parent, prefix, true);
}

}
