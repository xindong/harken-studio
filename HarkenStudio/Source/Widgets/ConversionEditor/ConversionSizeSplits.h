#pragma once

#include <QAbstractScrollArea>
#include <QVector>

#include <optional>

class QComboBox;
class QLineEdit;

namespace harken::studio {

class AxisIndicatorButton;

QString conversionSizeText(quint64 bytes);
QString exactConversionSizeText(quint64 bytes);

class ConversionSizeField final : public QWidget
{
public:
    ConversionSizeField(quint64 bytes, QWidget* parent);
    std::optional<quint64> bytes() const;

private:
    QLineEdit* value_;
    QComboBox* unit_;
    std::optional<quint64> bytes_;
};

class ConversionSizeSplits final : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit ConversionSizeSplits(QWidget* parent);
    void setLimits(QVector<quint64> limits);
    void setSelectedRange(int index);
    bool canSplit(int index) const;
    void requestSplit(int index);

signals:
    void rangeSelected(int index);
    void splitRequested(int index, quint64 bytes);
    void editRequested(int index, quint64 bytes);
    void boundaryPreviewed(int index, quint64 bytes);
    void boundaryMoved(int index, quint64 bytes);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct TickLabel {
        QRect rect;
        QString text;
    };
    struct SplitTarget {
        enum Part { Scale, Block, Divider } part;
        int index;
        int canvasX;
        std::optional<quint64> splitBytes;
    };
    QVector<TickLabel> visibleTicks() const;
    int barHeight() const;
    int edge(int index) const;
    QRect boundaryRect(int index) const;
    QRect rangeRect(int index) const;
    int rangeAt(int x) const;
    int activeBoundary() const;
    std::optional<SplitTarget> targetAt(const QPoint& point) const;
    quint64 valueInRange(int index, double fraction) const;
    void updateGeometryForLimits();
    void updateHover(QPoint position);
    void cancelDrag();

    QVector<quint64> limits_;
    AxisIndicatorButton* addMarker_;
    int barTop_ = 0;
    QRect scaleBand_;
    int minimumSpan_ = 1;
    int axisWidth_ = 1;
    int canvasWidth_ = 1;
    int selected_ = -1;
    QPoint hoverPosition_{-1, -1};
    std::optional<SplitTarget> hover_;
    int dragging_ = -1;
    int dragOffset_ = 0;
    quint64 original_ = 0;
};

}
