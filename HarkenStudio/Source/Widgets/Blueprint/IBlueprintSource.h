
#pragma once

#include "Core/ProjectTypes.h"
#include "../Common/WaveformRenderer.h"
#include "NodeContentInfo.h"

#include <QObject>
#include <QString>
#include <QPointF>

#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

class QMenu;
class QPoint;

namespace VNX {

enum class BlueprintNodeKind : uint8_t {
    ProjectItem = 0,
    Bus         = 1,
};

enum class BlueprintNodeRole : uint8_t {
    Self     = 0,
    Source   = 1,
    Parent   = 2,
    Bus        = 3,
    ReceiveBus = 4,
};

struct BlueprintNodeDesc {
    BlueprintNodeKind kind         = BlueprintNodeKind::ProjectItem;
    uint32_t          id           = 0;
    QString           title;
    QString           subtitle;
    BlueprintNodeRole role         = BlueprintNodeRole::Self;
    QPointF           position;
    bool              hasPosition  = false;
    bool              draggable    = true;
    qreal             opacity      = 1.0;
    bool              waveformMode = false;
    QString           mediaPath;
    std::optional<WaveformFrameRange> waveformRange;
    int               overrideBadge   = -1;
    int               receiveBadge    = -1;
    NodeContentInfo   content;
    ProjectItemType   itemType = ProjectItemType::None;
    bool              outputBusOverride = false;
    float             height     = 0.0f;
    std::optional<size_t> occurrenceIndex;
    int               portPairCount = 1;
};

struct BlueprintEdgeDesc {
    BlueprintNodeKind fromKind = BlueprintNodeKind::ProjectItem;
    uint32_t          fromId   = 0;
    BlueprintNodeKind toKind   = BlueprintNodeKind::ProjectItem;
    uint32_t          toId     = 0;
    uint8_t           edgeKind = 0;
    QString           label;
    BlueprintNodeRole styleRole  = BlueprintNodeRole::Self;
    std::optional<size_t> fromOccurrenceIndex;
    std::optional<size_t> toOccurrenceIndex;
    int               fromPortPairIndex = 0;
    int               toPortPairIndex = 0;
    NodeContentInfo   gainContent;
};

class IBlueprintSource : public QObject {
    Q_OBJECT
public:
    enum class Scope : uint8_t {
        Project      = 0,
        Drill        = 1,
        Focus        = 2,
        SingleObject = 3,
    };

    explicit IBlueprintSource(QObject* parent = nullptr) : QObject(parent) {}
    ~IBlueprintSource() override = default;

    virtual Scope                          scope() const = 0;
    virtual std::vector<BlueprintNodeDesc> buildNodes() = 0;
    virtual std::vector<BlueprintEdgeDesc> buildEdges() = 0;

    virtual bool supportsEditing() const { return false; }

    virtual bool onConnect(uint32_t , uint32_t ,
                           const QString& ,
                           const QString& ,
                           const QString& ) { return false; }
    virtual bool onDisconnect(uint32_t , uint32_t ,
                              const QString& ,
                              const QString& ,
                              const QString& ) { return false; }
    virtual bool onDelete(BlueprintNodeKind , uint32_t ) { return false; }

    virtual void contextMenu(QMenu& ,
                             const QPoint& ,
                             BlueprintNodeKind ,
                             uint32_t ) {}

signals:
    void sourceChanged();
};

}
