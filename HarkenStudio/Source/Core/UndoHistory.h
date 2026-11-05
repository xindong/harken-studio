#pragma once

#include <QObject>
#include <QString>

#include <memory>
#include <vector>

namespace VNX {

class UndoCommand
{
public:
    explicit UndoCommand(UndoCommand* parent = nullptr);
    explicit UndoCommand(const QString& text, UndoCommand* parent = nullptr);
    virtual ~UndoCommand();

    UndoCommand(const UndoCommand&) = delete;
    UndoCommand& operator=(const UndoCommand&) = delete;

    virtual void undo();
    virtual void redo();
    virtual int id() const { return -1; }
    virtual bool mergeWith(const UndoCommand*) { return false; }

    QString text() const { return text_; }
    QString actionText() const { return actionText_; }
    void setText(const QString& text);
    bool isObsolete() const { return obsolete_; }
    void setObsolete(bool obsolete) { obsolete_ = obsolete; }
    int childCount() const { return static_cast<int>(children_.size()); }
    const UndoCommand* child(int index) const;

private:
    friend class UndoStack;
    void adopt(UndoCommand* command);

    QString text_;
    QString actionText_;
    UndoCommand* parent_ = nullptr;
    std::vector<UndoCommand*> children_;
    bool obsolete_ = false;
};

class UndoStack : public QObject
{
    Q_OBJECT

public:
    explicit UndoStack(QObject* parent = nullptr);
    ~UndoStack() override;

    void push(UndoCommand* command);
    void undo();
    void redo();
    void clear();
    void beginMacro(const QString& text);
    void endMacro();
    void setClean();
    void resetClean();
    void setUndoLimit(int limit);
    void setIndex(int index);

    int count() const { return static_cast<int>(commands_.size()); }
    int index() const { return index_; }
    int cleanIndex() const { return cleanIndex_; }
    int undoLimit() const { return undoLimit_; }
    bool isClean() const { return macros_.empty() && index_ == cleanIndex_; }
    bool canUndo() const { return macros_.empty() && index_ > 0; }
    bool canRedo() const { return macros_.empty() && index_ < count(); }
    QString undoText() const;
    QString redoText() const;
    QString text(int index) const;
    const UndoCommand* command(int index) const;

signals:
    void canUndoChanged(bool available);
    void canRedoChanged(bool available);
    void cleanChanged(bool clean);
    void indexChanged(int index);
    void undoTextChanged(const QString& text);
    void redoTextChanged(const QString& text);

private:
    struct State {
        int index;
        bool clean;
        bool canUndo;
        bool canRedo;
        QString undoText;
        QString redoText;
    };

    State state() const;
    void publish(const State& before);
    void discardRedo();
    void trim();
    void remove(int index);

    std::vector<std::unique_ptr<UndoCommand>> commands_;
    std::vector<UndoCommand*> macros_;
    int index_ = 0;
    int cleanIndex_ = 0;
    int undoLimit_ = 0;
};

}
