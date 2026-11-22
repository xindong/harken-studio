#include "CurvePresetMenu.h"

#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"

#include <QMenu>
#include <QPixmap>
#include <QTransform>

#include <array>

namespace harken::studio {

QMenu* addCurvePresetMenu(QMenu* parentMenu,
                         const CurvePointDef& a, const CurvePointDef& b,
                         const CurveHandlePairDef& handles, CurveAxisScale scale,
                         std::function<void(CurvePreset)> apply)
{
    auto* menu = new QMenu(QMenu::tr("Presets"), parentMenu);
    menu->setObjectName(QStringLiteral("curvePresetMenu"));
    const bool descending = curveAxisCoordinate(scale, a.value) > curveAxisCoordinate(scale, b.value);
    const std::array names{
        descending ? QT_TR_NOOP("Cubic Ease-In") : QT_TR_NOOP("Cubic Ease-Out"),
        descending ? QT_TR_NOOP("Quadratic Ease-In") : QT_TR_NOOP("Quadratic Ease-Out"),
        QT_TR_NOOP("S-Curve"), QT_TR_NOOP("Linear"), QT_TR_NOOP("Inverse S-Curve"),
        descending ? QT_TR_NOOP("Quadratic Ease-Out") : QT_TR_NOOP("Quadratic Ease-In"),
        descending ? QT_TR_NOOP("Cubic Ease-Out") : QT_TR_NOOP("Cubic Ease-In")};
    for (const CurvePreset preset : kCurvePresets) {
        auto* action = menu->addAction(QMenu::tr(names[static_cast<size_t>(preset)]));
        action->setData(static_cast<int>(preset));
        action->setIconVisibleInMenu(true);
        action->setCheckable(true);
        action->setChecked(handles == curvePresetHandles(preset, a, b, scale)
            && (a.value != b.value || preset == CurvePreset::Linear));
        QObject::connect(action, &QAction::triggered, menu, [apply, preset] { apply(preset); });
    }
    const auto refreshIcons = [menu, descending] {
        static constexpr std::array assets{
            ThemeAssetIds::CurveCubicEaseOut, ThemeAssetIds::CurveQuadraticEaseOut, ThemeAssetIds::CurveBezierSCurve,
            ThemeAssetIds::CurveLinear, ThemeAssetIds::CurveInverseBezierSCurve,
            ThemeAssetIds::CurveQuadraticEaseIn, ThemeAssetIds::CurveCubicEaseIn};
        auto& theme = ThemeManager::instance();
        const QSize size(theme.metrics().iconExtent, theme.metrics().iconExtent);
        for (auto* action : menu->actions()) {
            auto icon = theme.resolveIcon(QString::fromLatin1(assets[action->data().toInt()])).icon;
            if (descending)
                icon = QIcon(icon.pixmap(size, menu->devicePixelRatioF())
                    .transformed(QTransform::fromScale(-1.0, 1.0)));
            action->setIcon(icon);
        }
    };
    refreshIcons();
    QObject::connect(menu, &QMenu::aboutToShow, menu, refreshIcons);
    QObject::connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, menu, refreshIcons);
    parentMenu->addMenu(menu);
    return menu;
}

}
