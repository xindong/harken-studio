
#pragma once

#include <QUndoCommand>
#include <QPointF>
#include <QString>

#include <cstdint>

#include "../../Core/ProjectTypes.h"
#include "../../Core/FieldGroupEditing.h"

namespace VNX {

class BlueprintConnectCommand : public QUndoCommand
{
public:
    enum class Effective { Containment, BusRoute, Override };

    BlueprintConnectCommand(uint32_t fromItemId,
                            uint32_t toId,
                            Effective effectiveType,
                            QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;
    QString error() const;

private:
    QUuid groupDefinition_;
    std::optional<FieldGroupEditResult> groupEdit_;
    uint32_t fromItemId_ = 0;
    uint32_t toId_       = 0;
    Effective effective_ = Effective::Containment;

    uint32_t  oldParentItemId_ = 0;
    ProjectItem* oldParentRaw_ = nullptr;
    uint32_t  oldOutputBusId_ = 0;
    uint16_t  oldOverrideBits_ = 0;
    bool      oldOutputBusOverride_ = false;

    bool firstRedo_ = true;
};

class BlueprintDisconnectCommand : public QUndoCommand
{
public:
    enum class Kind { Containment, BusRoute, Override };

    BlueprintDisconnectCommand(uint32_t fromItemId,
                               uint32_t toId,
                               Kind kind,
                               QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;
    QString error() const;

private:
    QUuid groupDefinition_;
    std::optional<FieldGroupEditResult> groupEdit_;
    uint32_t fromItemId_ = 0;
    uint32_t toId_       = 0;
    Kind     kind_       = Kind::Containment;

    ProjectItem* oldParentRaw_ = nullptr;
    uint32_t  oldOutputBusId_ = 0;
    uint16_t  oldOverrideBits_ = 0;
    bool      oldOutputBusOverride_ = false;

    bool firstRedo_ = true;
};

class BlueprintOverrideToggleCommand : public QUndoCommand
{
public:
    BlueprintOverrideToggleCommand(uint32_t itemId,
                                   bool newOverride,
                                   uint32_t autoRouteToBusId,
                                   QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    uint32_t itemId_ = 0;
    bool     newOverride_ = false;
    uint32_t autoRouteToBusId_ = 0;

    uint32_t oldOutputBusId_ = 0;
    uint16_t oldOverrideBits_ = 0;
    bool     oldOutputBusOverride_ = false;

    bool firstRedo_ = true;
};

class BlueprintSendConnectCommand : public QUndoCommand
{
public:
    BlueprintSendConnectCommand(uint32_t fromBusId,
                                uint32_t targetBusId,
                                QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QUuid groupDefinition_;
    std::optional<FieldGroupEditResult> groupEdit_;
    uint32_t fromBusId_   = 0;
    uint32_t targetBusId_ = 0;
    bool     applied_     = false;

    bool firstRedo_ = true;
};

class BlueprintSendDisconnectCommand : public QUndoCommand
{
public:
    BlueprintSendDisconnectCommand(uint32_t fromBusId,
                                   uint32_t targetBusId,
                                   QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QUuid groupDefinition_;
    std::optional<FieldGroupEditResult> groupEdit_;
    uint32_t fromBusId_   = 0;
    uint32_t targetBusId_ = 0;

    float sendLevel_ = 0.0f;
    bool  hadSend_   = false;

    bool firstRedo_ = true;
};

class BlueprintNodePositionCommand : public QUndoCommand
{
public:
    BlueprintNodePositionCommand(uint32_t itemId,
                                 const QPointF& oldPos,
                                 const QPointF& newPos,
                                 QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    uint32_t itemId_ = 0;
    QPointF  oldPos_;
    QPointF  newPos_;

    bool firstRedo_ = true;
};

}
