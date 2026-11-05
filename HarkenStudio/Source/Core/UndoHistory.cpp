#include "UndoHistory.h"

#include <QDebug>

#include <algorithm>

namespace VNX {

UndoCommand::UndoCommand(UndoCommand* parent)
{
    if (parent) parent->adopt(this);
}

UndoCommand::UndoCommand(const QString& text, UndoCommand* parent)
    : UndoCommand(parent)
{
    setText(text);
}

UndoCommand::~UndoCommand()
{
    if (parent_) std::erase(parent_->children_, this);
    while (!children_.empty()) delete children_.back();
}

void UndoCommand::adopt(UndoCommand* command)
{
    if (command->parent_) std::erase(command->parent_->children_, command);
    command->parent_ = this;
    children_.push_back(command);
}

void UndoCommand::undo()
{
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) (*it)->undo();
}

void UndoCommand::redo()
{
    for (auto* command : children_) command->redo();
}

void UndoCommand::setText(const QString& text)
{
    const auto separator = text.indexOf(QLatin1Char('\n'));
    text_ = separator < 0 ? text : text.left(separator);
    actionText_ = separator < 0 ? text : text.mid(separator + 1);
}

const UndoCommand* UndoCommand::child(int index) const
{
    return index >= 0 && index < childCount() ? children_[index] : nullptr;
}

UndoStack::UndoStack(QObject* parent) : QObject(parent) {}
UndoStack::~UndoStack() = default;

UndoStack::State UndoStack::state() const
{
    return {index_, isClean(), canUndo(), canRedo(), undoText(), redoText()};
}

void UndoStack::publish(const State& before)
{
    const auto after = state();
    if (before.index != after.index) emit indexChanged(after.index);
    if (before.clean != after.clean) emit cleanChanged(after.clean);
    if (before.canUndo != after.canUndo) emit canUndoChanged(after.canUndo);
    if (before.canRedo != after.canRedo) emit canRedoChanged(after.canRedo);
    if (before.undoText != after.undoText) emit undoTextChanged(after.undoText);
    if (before.redoText != after.redoText) emit redoTextChanged(after.redoText);
}

void UndoStack::discardRedo()
{
    if (cleanIndex_ > index_) cleanIndex_ = -1;
    commands_.erase(commands_.begin() + index_, commands_.end());
}

void UndoStack::trim()
{
    if (undoLimit_ <= 0 || count() <= undoLimit_) return;
    const int removed = count() - undoLimit_;
    commands_.erase(commands_.begin(), commands_.begin() + removed);
    index_ -= removed;
    cleanIndex_ = cleanIndex_ < removed ? -1 : cleanIndex_ - removed;
}

void UndoStack::remove(int index)
{
    if (cleanIndex_ > index) cleanIndex_ = -1;
    commands_.erase(commands_.begin() + index);
    if (index_ > index) --index_;
}

void UndoStack::push(UndoCommand* command)
{
    const auto before = state();
    if (!command->isObsolete()) command->redo();
    if (macros_.empty()) discardRedo();
    if (!macros_.empty()) {
        auto* macro = macros_.back();
        auto* previous = macro->children_.empty() ? nullptr : macro->children_.back();
        if (previous && previous->id() != -1 && previous->id() == command->id()
            && previous->mergeWith(command)) {
            delete command;
            if (previous->isObsolete()) delete previous;
        } else if (command->isObsolete()) {
            delete command;
        } else {
            macro->adopt(command);
        }
        return;
    }
    auto* previous = index_ > 0 ? commands_[index_ - 1].get() : nullptr;
    if (previous && cleanIndex_ != index_ && previous->id() != -1
        && previous->id() == command->id() && previous->mergeWith(command)) {
        delete command;
        if (previous->isObsolete()) remove(index_ - 1);
    } else if (command->isObsolete()) {
        delete command;
    } else {
        commands_.emplace_back(command);
        ++index_;
        trim();
    }
    if (before.index == index_) emit indexChanged(index_);
    publish(before);
}

void UndoStack::undo()
{
    if (!canUndo()) return;
    const auto before = state();
    const int position = index_ - 1;
    auto* command = commands_[position].get();
    if (!command->isObsolete()) command->undo();
    --index_;
    if (command->isObsolete()) remove(position);
    publish(before);
}

void UndoStack::redo()
{
    if (!canRedo()) return;
    const auto before = state();
    auto* command = commands_[index_].get();
    if (!command->isObsolete()) command->redo();
    if (command->isObsolete()) remove(index_);
    else ++index_;
    publish(before);
}

void UndoStack::clear()
{
    if (commands_.empty()) return;
    const auto before = state();
    macros_.clear();
    commands_.clear();
    index_ = 0;
    cleanIndex_ = 0;
    publish(before);
}

void UndoStack::beginMacro(const QString& text)
{
    const auto before = state();
    UndoCommand* macro;
    if (macros_.empty()) {
        discardRedo();
        commands_.push_back(std::make_unique<UndoCommand>(text));
        macro = commands_.back().get();
    } else {
        macro = new UndoCommand(text, macros_.back());
    }
    macros_.push_back(macro);
    publish(before);
}

void UndoStack::endMacro()
{
    if (macros_.empty()) return;
    const auto before = state();
    macros_.pop_back();
    if (macros_.empty()) {
        ++index_;
        trim();
    }
    publish(before);
}

void UndoStack::setClean()
{
    if (!macros_.empty()) return;
    const auto before = state();
    cleanIndex_ = index_;
    publish(before);
}

void UndoStack::resetClean()
{
    const auto before = state();
    cleanIndex_ = -1;
    publish(before);
}

void UndoStack::setUndoLimit(int limit)
{
    if (!commands_.empty()) {
        qWarning() << "Undo history limit can only change while history is empty.";
        return;
    }
    undoLimit_ = std::max(0, limit);
}

void UndoStack::setIndex(int index)
{
    if (!macros_.empty()) return;
    const auto before = state();
    index = std::clamp(index, 0, count());
    while (index_ > index) {
        auto* command = commands_[index_ - 1].get();
        command->undo();
        --index_;
        if (command->isObsolete()) remove(index_);
    }
    while (index_ < index) {
        auto* command = commands_[index_].get();
        if (!command->isObsolete()) command->redo();
        if (command->isObsolete()) {
            remove(index_);
            --index;
        } else {
            ++index_;
        }
    }
    publish(before);
}

QString UndoStack::undoText() const
{
    return canUndo() ? commands_[index_ - 1]->text() : QString();
}

QString UndoStack::redoText() const
{
    return canRedo() ? commands_[index_]->text() : QString();
}

QString UndoStack::text(int index) const
{
    const auto* entry = command(index);
    return entry ? entry->text() : QString();
}

const UndoCommand* UndoStack::command(int index) const
{
    return index >= 0 && index < count() ? commands_[index].get() : nullptr;
}

}
