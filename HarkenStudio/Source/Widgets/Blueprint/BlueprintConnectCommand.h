
#pragma once

#include "Core/UndoHistory.h"
#include <QString>

#include <cstdint>

#include "../../Core/ProjectTypes.h"
#include "../../Core/FieldGroupEditing.h"
#include "../../Core/BusRouteGraph.h"

namespace harken::studio {

class BlueprintConnectCommand : public harken::studio::UndoCommand
{
public:
    enum class Effective { Containment, BusRoute, Override };

    BlueprintConnectCommand(uint32_t fromItemId,
                            uint32_t toId,
                            Effective effectiveType,
                            harken::studio::UndoCommand* parent = nullptr);

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

class BlueprintDisconnectCommand : public harken::studio::UndoCommand
{
public:
    enum class Kind { Containment, BusRoute, Override };

    BlueprintDisconnectCommand(uint32_t fromItemId,
                               uint32_t toId,
                               Kind kind,
                               harken::studio::UndoCommand* parent = nullptr);

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

class BlueprintOverrideToggleCommand : public harken::studio::UndoCommand
{
public:
    BlueprintOverrideToggleCommand(uint32_t itemId,
                                   bool newOverride,
                                   uint32_t autoRouteToBusId,
                                   harken::studio::UndoCommand* parent = nullptr);

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

class BlueprintSendConnectCommand : public harken::studio::UndoCommand
{
public:
    BlueprintSendConnectCommand(uint32_t fromBusId,
                                uint32_t targetBusId,
                                SendType type = SendType::Send, bool duckTarget = false,
                                harken::studio::UndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QUuid groupDefinition_;
    std::optional<FieldGroupEditResult> groupEdit_;
    uint32_t fromBusId_   = 0;
    uint32_t targetBusId_ = 0;
    SendType type_ = SendType::Send;
    bool duckTarget_ = false;
    std::optional<BusSendState> before_;
    std::optional<BusSendState> after_;

    bool firstRedo_ = true;
};

class BlueprintSendDisconnectCommand : public harken::studio::UndoCommand
{
public:
    BlueprintSendDisconnectCommand(uint32_t fromBusId,
                                   uint32_t targetBusId,
                                   SendType type = SendType::Send, bool duckTarget = false,
                                   harken::studio::UndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QUuid groupDefinition_;
    std::optional<FieldGroupEditResult> groupEdit_;
    uint32_t fromBusId_   = 0;
    uint32_t targetBusId_ = 0;

    SendType type_ = SendType::Send;
    bool duckTarget_ = false;
    std::optional<BusSendState> before_;
    std::optional<BusSendState> after_;

    bool firstRedo_ = true;
};

}
