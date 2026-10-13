#include "MonitorCursorMark.h"

#include "Core/ThemeManager.h"

#include <QPainter>

namespace VNX {

MonitorCursorMark::MonitorCursorMark(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(80, 24);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void MonitorCursorMark::updateData(const MonitorCursorSnapshot& snapshot)
{
    snapshot_ = snapshot;
    update();
}

QString MonitorCursorMark::statusText() const
{
    if (!snapshot_.executeFrame.has_value())
        return QString::fromUtf8(MonitorCursorModel::unreadinessText());
    if (snapshot_.historyUnavailable)
        return QString::fromUtf8(MonitorCursorModel::historyUnavailableText());
    return QString::fromUtf8(MonitorCursorModel::modeText(snapshot_.mode));
}

QString MonitorCursorMark::uncertaintyLabel() const
{
    return QString::fromUtf8(MonitorCursorModel::uncertaintyText(displayedUncertainty()));
}

MonitorCursorUncertaintyKind MonitorCursorMark::displayedUncertainty() const
{
    if (!snapshot_.executeFrame.has_value() || snapshot_.historyUnavailable)
        return MonitorCursorUncertaintyKind::Unknown;
    return snapshot_.uncertaintyKind;
}

QColor MonitorCursorMark::uncertaintyColor() const
{
    const auto snapshot = ThemeManager::instance().snapshot();
    const auto& monitor = snapshot->domains.monitor;
    switch (displayedUncertainty())
    {
    case MonitorCursorUncertaintyKind::Exact:
        return monitor.cpuSafe;
    case MonitorCursorUncertaintyKind::Estimate:
        return monitor.cpuWarning;
    case MonitorCursorUncertaintyKind::Unknown:
        return monitor.underrunNone;
    }
    return monitor.underrunNone;
}

bool MonitorCursorMark::showsLine() const
{
    return snapshot_.executeFrame.has_value() && !snapshot_.historyUnavailable;
}

void MonitorCursorMark::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const auto snapshot = ThemeManager::instance().snapshot();
    const auto& ui = snapshot->ui.colors;
    painter.fillRect(rect(), ui.bgPrimary);
    if (showsLine())
    {
        painter.setPen(QPen(uncertaintyColor(), 2));
        const int x = width() / 2;
        painter.drawLine(x, 0, x, height());
    }
    painter.setPen(ui.textPrimary);
    const auto text = statusText() + QStringLiteral("  ") + uncertaintyLabel();
    painter.drawText(rect().adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
}

} // namespace VNX
