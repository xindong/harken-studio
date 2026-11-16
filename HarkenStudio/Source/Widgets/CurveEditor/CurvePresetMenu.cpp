#include "CurvePresetMenu.h"

#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"

#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionMenuItem>

#include <array>

namespace VNX {
namespace {

class CurvePresetMenu : public QMenu
{
public:
    CurvePresetMenu(QMenu* parent, const CurvePointDef& a, const CurvePointDef& b,
                    const CurveHandlePairDef& handles, CurveAxisScale scale,
                    const std::function<void(CurvePreset)>& apply)
        : QMenu(tr("Presets"), parent)
        , descending_(curveAxisCoordinate(scale, a.value) > curveAxisCoordinate(scale, b.value))
    {
        setObjectName(QStringLiteral("curvePresetMenu"));
        static constexpr std::array names{
            QT_TR_NOOP("Strong convex"), QT_TR_NOOP("Convex"), QT_TR_NOOP("S curve"),
            QT_TR_NOOP("Linear"), QT_TR_NOOP("Mirrored S curve"),
            QT_TR_NOOP("Concave"), QT_TR_NOOP("Strong concave")};
        for (const CurvePreset preset : kCurvePresets) {
            auto* action = addAction(tr(names[static_cast<size_t>(preset)]));
            action->setData(static_cast<int>(preset));
            action->setCheckable(true);
            action->setChecked(handles == curvePresetHandles(preset, a, b, scale)
                && (a.value != b.value || preset == CurvePreset::Linear));
            connect(action, &QAction::triggered, this, [apply, preset] { apply(preset); });
        }
        const auto fitWidth = [this] {
            const auto& metrics = ThemeManager::instance().metrics();
            setFixedWidth(metrics.popup.iconColumnWidth + metrics.iconExtent
                + 2 * metrics.control.textPaddingX);
            update();
        };
        fitWidth();
        connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, this, fitWidth);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        QStyleOption panel;
        panel.initFrom(this);
        style()->drawPrimitive(QStyle::PE_PanelMenu, &panel, &painter, this);
        const auto theme = ThemeManager::instance().snapshot();
        const auto& metrics = theme->ui.metrics;
        static constexpr std::array assets{
            ThemeAssetIds::CurveLog3, ThemeAssetIds::CurveConvex, ThemeAssetIds::CurveSCurve,
            ThemeAssetIds::CurveLinear, ThemeAssetIds::CurveInverseSCurve,
            ThemeAssetIds::CurveConcave, ThemeAssetIds::CurveExp3};
        for (auto* action : actions()) {
            QStyleOptionMenuItem option;
            initStyleOption(&option, action);
            option.rect = actionGeometry(action).intersected(contentsRect());
            option.text.clear();
            style()->drawControl(QStyle::CE_MenuItem, &option, &painter, this);
            const QRect content = option.rect.adjusted(
                metrics.popup.iconColumnWidth + metrics.control.textPaddingX,
                0, -metrics.control.textPaddingX, 0);
            const QSize size(metrics.iconExtent, metrics.iconExtent);
            const QRect iconRect = QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, size, content);
            const auto visual = ThemeManager::instance().widgetAppearance().visual(this, option.state, u"item");
            const QColor color = visual.foreground.isValid() ? visual.foreground : option.palette.color(
                action->isEnabled() ? QPalette::Active : QPalette::Disabled,
                option.state.testFlag(QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::ButtonText);
            const auto icon = ThemeManager::instance().resolveIcon(
                QString::fromLatin1(assets[action->data().toInt()]),
                {size, devicePixelRatioF(), 0, color}).icon;
            painter.save();
            painter.translate(iconRect.left() + (descending_ ? iconRect.width() : 0), iconRect.top());
            if (descending_)
                painter.scale(-1.0, 1.0);
            icon.paint(&painter, QRect(QPoint(), size));
            painter.restore();
        }
    }

private:
    bool descending_;
};

}

QMenu* addCurvePresetMenu(QMenu* parentMenu,
                         const CurvePointDef& a, const CurvePointDef& b,
                         const CurveHandlePairDef& handles, CurveAxisScale scale,
                         std::function<void(CurvePreset)> apply)
{
    auto* menu = new CurvePresetMenu(parentMenu, a, b, handles, scale, apply);
    parentMenu->addMenu(menu);
    return menu;
}

}
