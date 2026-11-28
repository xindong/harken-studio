#pragma once

#include "Core/PlatformLocalState.h"

#include <QUuid>
#include <QWidget>

class QGridLayout;

#include <functional>
#include <optional>
#include <vector>

namespace harken::studio {

struct PropertyDescriptor;

struct PlatformMarkValue
{
    QString platform;
    QVariant value;
};

using PlatformValuesView = std::function<void(QGridLayout*, QWidget*, const QList<PlatformMarkValue>&)>;

class PlatformLocalMarker final : public QWidget
{
    Q_OBJECT

public:
    enum class State { None, OtherPlatform, CurrentPlatform };

    explicit PlatformLocalMarker(QWidget* parent = nullptr);

    void setValueFormatter(std::function<QString(const QVariant&)> formatter);
    void setValuesView(PlatformValuesView view);
    void bind(const ProjectItem* item, const std::optional<PlatformLocalAddress>& address);
    void setEditable(bool editable);
    void setEdges(Qt::Edges edges);
    void refresh();
    State state() const { return state_; }

    static std::function<QString(const QVariant&)> formatterFor(const PropertyDescriptor& descriptor);

signals:
    void localChanged();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    ProjectItem* boundItem() const;
    bool interactive() const;
    void showValues();
    void changeLocal(bool local);

    QUuid item_;
    std::optional<PlatformLocalAddress> address_;
    std::function<QString(const QVariant&)> formatter_;
    PlatformValuesView valuesView_;
    State state_ = State::None;
    bool editable_ = true;
    bool hovered_ = false;
    Qt::Edges edges_ = Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge;
};

enum class PlatformMarkerHeight { Control, Field };

QWidget* attachPlatformMarker(QWidget* control, PlatformLocalMarker* marker, QWidget* parent,
                              PlatformMarkerHeight height = PlatformMarkerHeight::Control);

void showPlatformComparison(QGridLayout* layout, QWidget* popup, const QString& rowTitle,
                            const QList<PlatformMarkValue>& platforms, const std::vector<QStringList>& rows,
                            const QString& identical, Qt::Alignment valueAlignment);

}
