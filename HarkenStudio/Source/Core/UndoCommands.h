
#pragma once

#include <QUndoCommand>
#include <QUndoStack>
#include <QAction>
#include <QJsonObject>
#include <QVariant>
#include <QString>
#include <QPointF>
#include <QVector3D>
#include <functional>
#include <memory>
#include <vector>

#include "ProjectTypes.h"
#include "Components/VBus.h"

namespace VNX {

class TransactionalUndoCommand : public QUndoCommand
{
public:
    explicit TransactionalUndoCommand(const QString& text, QUndoCommand* parent = nullptr,
                                      bool applied = true)
        : QUndoCommand(text, parent), applied_(applied) {}

    bool tryReplay(bool redo);
    void undo() final { tryReplay(false); }
    void redo() final { tryReplay(true); }

protected:
    virtual bool apply(bool redo) = 0;

private:
    bool applied_ = true;
};

class UndoRedoManager : public QObject
{
    Q_OBJECT

public:
    static UndoRedoManager& instance();
    
    QUndoStack* undoStack() { return &undoStack_; }
    
    QAction* createUndoAction(QObject* parent, const QString& prefix = QString());
    QAction* createRedoAction(QObject* parent, const QString& prefix = QString());
    
    void push(QUndoCommand* command);
    
    void beginMacro(const QString& text);
    void endMacro();
    void clear();
    
    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();
    bool isClean() const { return undoStack_.isClean(); }
    void setClean() { undoStack_.setClean(); }

signals:
    void canUndoChanged(bool canUndo);
    void canRedoChanged(bool canRedo);
    void cleanChanged(bool clean);
    void indexChanged(int idx);

private:
    UndoRedoManager();
    ~UndoRedoManager() override = default;

    QAction* createReplayAction(QObject* parent, const QString& prefix, bool redo);
    bool replay(bool redo);
    void refreshReplayAvailability();
    
    QUndoStack undoStack_;
};

class PropertyChangeCommand : public QUndoCommand
{
public:
    PropertyChangeCommand(
        QObject* target,
        const char* propertyName,
        const QVariant& oldValue,
        const QVariant& newValue,
        const QString& text = QString(),
        QUndoCommand* parent = nullptr
    );
    
    void undo() override;
    void redo() override;
    
    int id() const override { return 1001; }
    bool mergeWith(const QUndoCommand* other) override;

private:
    QObject* target_;
    QByteArray propertyName_;
    QVariant oldValue_;
    QVariant newValue_;
};

class ProjectItemCommand : public QUndoCommand
{
public:
    enum Operation { Add, Remove, Rename, Move };
    
    ProjectItemCommand(
        Operation op,
        const QString& itemId,
        const QString& parentId,
        const QVariant& itemData,
        const QVariant& previousData,
        std::function<void(Operation, const QString&, const QString&, const QVariant&)> executeFn,
        const QString& text = QString(),
        QUndoCommand* parent = nullptr
    );
    
    void undo() override;
    void redo() override;

private:
    Operation op_;
    QString itemId_;
    QString parentId_;
    QVariant itemData_;
    QVariant previousData_;
    std::function<void(Operation, const QString&, const QString&, const QVariant&)> executeFn_;
};

class Scene3DCommand : public QUndoCommand
{
public:
    enum ObjectType { Emitter, Listener, Obstruction };
    
    Scene3DCommand(
        ObjectType objectType,
        int objectId,
        const QVector3D& oldPosition,
        const QVector3D& newPosition,
        std::function<void(ObjectType, int, const QVector3D&)> applyFn,
        const QString& text = QString(),
        QUndoCommand* parent = nullptr
    );
    
    void undo() override;
    void redo() override;
    
    int id() const override { return 1005; }
    bool mergeWith(const QUndoCommand* other) override;

private:
    ObjectType objectType_;
    int objectId_;
    QVector3D oldPosition_;
    QVector3D newPosition_;
    std::function<void(ObjectType, int, const QVector3D&)> applyFn_;
};

class DeleteProjectItemCommand : public QUndoCommand
{
public:
    DeleteProjectItemCommand(
        ProjectItem* item,
        QUndoCommand* parent = nullptr
    );

    void undo() override;
    void redo() override;

private:
    bool isPackValid() const;

    QString itemName_;
    ProjectItem::Type itemType_;
    uint32_t sourceID_;
    uint32_t objectID_;
    bool expanded_;

    std::shared_ptr<ProjectItem> parentRef_;
    ProjectItem* parent_;
    QString packGuid_;
    int originalIndex_;

    bool      isTopLevelBinding_ = false;
    RootBindingSlot rootSlot_;

    std::shared_ptr<ProjectItem> savedItem_;

    DeletedSubtreeRefLog refLog_;

    bool firstRedo_ = true;
};


class CreateProjectItemCommand : public QUndoCommand
{
public:
    CreateProjectItemCommand(
        ProjectItem* parent,
        std::shared_ptr<ProjectItem> newItem,
        QUndoCommand* undoParent = nullptr
    );

    void undo() override;
    void redo() override;

private:
    ProjectItem* parent_;
    std::shared_ptr<ProjectItem> item_;
    DeletedSubtreeRefLog refLog_;
    bool      isTopLevelBinding_ = false;
    RootBindingSlot rootSlot_;
    bool firstRedo_ = true;
};

class RenameProjectItemCommand : public TransactionalUndoCommand
{
public:
    RenameProjectItemCommand(
        ProjectItem* item,
        const QString& oldName,
        const QString& newName,
        QUndoCommand* undoParent = nullptr
    );

private:
    bool apply(bool redo) override;
    ProjectItem* item_;
    QString oldName_;
    QString newName_;
};

class SetItemColorCommand : public QUndoCommand
{
public:
    SetItemColorCommand(
        ProjectItem* item,
        uint32_t oldColor,
        uint32_t newColor,
        QUndoCommand* undoParent = nullptr
    );

    void undo() override;
    void redo() override;

private:
    ProjectItem* item_;
    uint32_t oldColor_;
    uint32_t newColor_;
    bool firstRedo_ = true;
};


class MoveItemCommand : public TransactionalUndoCommand
{
public:
    MoveItemCommand(
        ProjectItem* item,
        ProjectItem* newParent,
        QUndoCommand* parent = nullptr
    );

    MoveItemCommand(
        ProjectItem* item,
        const QString& bucketRootFolder,
        QUndoCommand* parent = nullptr
    );

private:
    bool apply(bool redo) override;
    bool undoHierarchy();
    bool redoHierarchy();
    bool doMove(
        ProjectItem* item,
        ProjectItem* targetParent,
        int targetIndex = -1,
        const std::optional<SelectionEntry>& restoredDestination = std::nullopt,
        std::optional<SelectionEntry>* removedSource = nullptr,
        std::optional<SelectionEntry>* installedDestination = nullptr,
        std::vector<SelectionEntry>* removedSources = nullptr,
        bool restoreEmptySourceOccurrences = false,
        DeletedSubtreeRefLog* outLog = nullptr,
        bool createDefaultClip = true);
    void prepareTrackTimeline(ProjectItem* targetParent);
    void applyTrackTimeline(const std::optional<ClipLedger>& clips);
    void captureSelections();
    void applySelections(ProjectItem* parent, const std::optional<SelectionData>& value);
    void finishSelectionMove();

    std::shared_ptr<ProjectItem> itemRef_;
    ProjectItem* item_;

    ProjectItem* oldParent_;
    ProjectItem* newParent_;
    int originalIndex_;

    QString oldParentGuid_;
    QString newParentGuid_;

    uint32_t conflictSendTargetBusId_ = 0;
    float conflictSendLevel_ = 0.0f;

    bool wasTopLevelBinding_ = false;
    RootBindingSlot savedBindingSlot_;

    QString bucketRootFolder_;
    ParentRef savedParentRef_;
    bool createdBinding_ = false;

    std::optional<ClipLedger> oldTrackClips_;
    std::optional<ClipLedger> newTrackClips_;
    std::optional<DefinitionFieldGroups> oldFieldGroups_;
    std::optional<DefinitionFieldGroups> rootFieldGroups_;
    QString fieldGroupConversionError_;
    bool blocked_ = false;
    std::optional<SelectionData> oldParentSelections_;
    std::optional<SelectionData> newParentSelections_;
    std::optional<SelectionData> movedOldParentSelections_;
    std::optional<SelectionData> movedNewParentSelections_;
    bool capturedMovedSelections_ = false;
    QString timelineConversionError_;

    std::optional<SelectionEntry> oldOccurrence_;
    std::optional<SelectionEntry> newOccurrence_;
    std::vector<SelectionEntry> oldOccurrences_;
    bool capturedOldOccurrences_ = false;
    DeletedSubtreeRefLog playbackRefLog_;
    DeletedSubtreeRefLog redoPlaybackRefLog_;

    bool firstRedo_ = true;
};

bool pushMoveCommands(std::vector<std::unique_ptr<TransactionalUndoCommand>> commands,
                      const QString& text);
std::unique_ptr<TransactionalUndoCommand> makeMoveCommand(
    std::vector<std::unique_ptr<TransactionalUndoCommand>> commands, const QString& text);

class CreateParentContainerCommand : public TransactionalUndoCommand
{
public:
    CreateParentContainerCommand(const QList<ProjectItem*>& items,
                                 ProjectItem::Type type, const QString& name);
    ProjectItem* container() const { return container_; }

private:
    bool apply(bool redo) override;
    QList<ProjectItem*> items_;
    ProjectItem::Type type_;
    QString name_;
    ProjectItem* parent_ = nullptr;
    ProjectItem* container_ = nullptr;
    std::unique_ptr<QUndoCommand> creation_;
    std::unique_ptr<TransactionalUndoCommand> moves_;
};

class PastePropertiesCommand : public QUndoCommand
{
public:
    PastePropertiesCommand(const std::vector<ProjectItem*>& targets,
                           const ProjectItem* source,
                           QUndoCommand* undoParent = nullptr);

    void undo() override;
    void redo() override;

    int appliedCount() const { return static_cast<int>(snaps_.size()); }

private:
    struct Snap { ProjectItem* item = nullptr; QJsonObject before; QJsonObject after; };
    std::vector<Snap> snaps_;
};

class ContainerPropertyChangeCommand : public QUndoCommand
{
public:
    ContainerPropertyChangeCommand(
        ProjectItem* item,
        const QString& propertyName,
        const QVariant& oldValue,
        const QVariant& newValue,
        std::function<void(ProjectItem*, const QVariant&)> applyFn,
        const QString& text = QString(),
        QUndoCommand* parent = nullptr
    );
    
    void undo() override;
    void redo() override;
    
    int id() const override { return 1010; }
    bool mergeWith(const QUndoCommand* other) override;

private:
    ProjectItem* item_;
    QString propertyName_;
    QVariant oldValue_;
    QVariant newValue_;
    std::function<void(ProjectItem*, const QVariant&)> applyFn_;
    bool firstRedo_ = true;
};


class BusPropertyChangeCommand : public QUndoCommand
{
public:
    BusPropertyChangeCommand(
        uint32_t busID,
        const QString& propertyName,
        const QVariant& oldValue,
        const QVariant& newValue,
        std::function<void(uint32_t, const QVariant&)> applyFn,
        const QString& text = QString(),
        QUndoCommand* parent = nullptr
    );
    
    void undo() override;
    void redo() override;
    
    int id() const override { return 1011; }
    bool mergeWith(const QUndoCommand* other) override;

private:
    uint32_t objectID_;
    QString propertyName_;
    QVariant oldValue_;
    QVariant newValue_;
    std::function<void(uint32_t, const QVariant&)> applyFn_;
    bool firstRedo_ = true;
};

class BusSendCommand : public QUndoCommand
{
public:
    BusSendCommand(
        uint32_t busID,
        uint32_t targetBusID,
        float sendLevel,
        bool isAdd,
        std::function<void(uint32_t, uint32_t, float)> addFn,
        std::function<void(uint32_t, uint32_t)> removeFn,
        const QString& text = QString(),
        QUndoCommand* parent = nullptr
    );

    void undo() override;
    void redo() override;

    int id() const override { return 1012; }

private:
    uint32_t objectID_;
    uint32_t targetBusId_;
    float sendLevel_;
    bool isAdd_;
    std::function<void(uint32_t, uint32_t, float)> addFn_;
    std::function<void(uint32_t, uint32_t)> removeFn_;
    bool firstRedo_ = true;
};

class SequenceReorderCommand : public QUndoCommand
{
public:
    SequenceReorderCommand(
        ProjectItem* item,
        const std::vector<std::shared_ptr<ProjectItem>>& oldOrder,
        const std::vector<std::shared_ptr<ProjectItem>>& newOrder,
        QUndoCommand* parent = nullptr
    );

    void undo() override;
    void redo() override;

    int id() const override { return 1013; }

private:
    void applyOrder(const std::vector<std::shared_ptr<ProjectItem>>& order);

    ProjectItem* item_;
    std::vector<std::shared_ptr<ProjectItem>> oldOrder_;
    std::vector<std::shared_ptr<ProjectItem>> newOrder_;
    bool firstRedo_ = true;
};

class SpatialPropertyChangeCommand : public QUndoCommand
{
public:
    SpatialPropertyChangeCommand(
        ProjectItem* item,
        const QString& propertyName,
        const QVariant& oldValue,
        const QVariant& newValue,
        std::function<void(ProjectItem*, const QVariant&)> applyFn,
        const QString& text = QString(),
        QUndoCommand* parent = nullptr
    );

    void undo() override;
    void redo() override;
    int id() const override { return 1050; }
    bool mergeWith(const QUndoCommand* other) override;

private:
    ProjectItem* item_;
    QString propertyName_;
    QVariant oldValue_;
    QVariant newValue_;
    std::function<void(ProjectItem*, const QVariant&)> applyFn_;
    bool firstRedo_ = true;
};


class AutoComplexUndo
{
public:
    explicit AutoComplexUndo(const QString& name)
        : name_(name) {}

    ~AutoComplexUndo()
    {
        if (state_ == State::Open)
            close();
    }

    AutoComplexUndo(const AutoComplexUndo&) = delete;
    AutoComplexUndo& operator=(const AutoComplexUndo&) = delete;

    bool open()
    {
        if (state_ != State::Initial)
            return false;
        UndoRedoManager::instance().beginMacro(name_);
        state_ = State::Open;
        return true;
    }

    bool close(const QString& overrideName = {})
    {
        if (state_ != State::Open)
            return false;
        Q_UNUSED(overrideName)
        UndoRedoManager::instance().endMacro();
        state_ = State::Closed;
        return true;
    }

    bool cancel()
    {
        if (state_ != State::Open)
            return false;
        UndoRedoManager::instance().endMacro();
        if (UndoRedoManager::instance().canUndo())
            UndoRedoManager::instance().undo();
        state_ = State::Cancelled;
        return true;
    }

    bool isOpen() const { return state_ == State::Open; }

private:
    enum class State { Initial, Open, Closed, Cancelled };
    QString name_;
    State state_ = State::Initial;
};


class RuntimeActionCommand : public QUndoCommand
{
public:
    RuntimeActionCommand(const QString& text,
                         std::function<void()> undoFn,
                         std::function<void()> redoFn,
                         int mergeId = 0,
                         QUndoCommand* parent = nullptr,
                         std::function<bool()> canReplay = {})
        : QUndoCommand(text, parent)
        , undoFn_(std::move(undoFn))
        , redoFn_(std::move(redoFn))
        , mergeId_(mergeId)
        , canReplay_(std::move(canReplay)) {}

    bool canReplay() const { return !canReplay_ || canReplay_(); }

    void undo() override { undoFn_(); }
    void redo() override {
        if (firstRedo_) { firstRedo_ = false; return; }
        redoFn_();
    }
    int id() const override { return mergeId_; }
    bool mergeWith(const QUndoCommand* other) override {
        auto* o = dynamic_cast<const RuntimeActionCommand*>(other);
        if (!o || o->mergeId_ != mergeId_ || mergeId_ == 0) return false;
        redoFn_ = o->redoFn_;
        setText(o->text());
        return true;
    }

private:
    std::function<void()> undoFn_;
    std::function<void()> redoFn_;
    int mergeId_ = 0;
    bool firstRedo_ = true;
    std::function<bool()> canReplay_;
};

}
