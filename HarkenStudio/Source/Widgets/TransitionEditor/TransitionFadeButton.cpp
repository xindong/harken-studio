#include "TransitionFadeButton.h"

#include "Core/CurveSemantics.h"
#include "Core/NumericFormat.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "Widgets/CurveEditor/CurveCanvas.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QStyle>
#include <QScreen>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace VNX {

namespace {

constexpr int kThumbWidth = 28;
constexpr int kThumbSamples = 16;
constexpr QSize kPopupSize(400, 280);

QString formatSeconds(float value)
{
    return formatDecimal(value, 3);
}

void paintFadeCurve(QPainter& p, const QRectF& area,
                    const TransitionFadeDef& fade, const QColor& color)
{
    p.setRenderHint(QPainter::Antialiasing);

    const QVector<CurvePointDef>& points = fade.curve;
    if (points.size() < 2)
        return;

    const auto mapPoint = [&](float x, float value) {
        const float nx = qBound(0.0f, x, 1.0f);
        const float ny = qBound(0.0f, value, 1.0f);
        return QPointF(area.left() + nx * area.width(), area.bottom() - ny * area.height());
    };
    QPolygonF poly;
    poly.append(mapPoint(points.front().x, points.front().value));
    for (int i = 0; i + 1 < points.size(); ++i) {
        const CurvePointDef& left = points[i];
        const CurvePointDef& right = points[i + 1];
        for (int step = 1; step < kThumbSamples; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(kThumbSamples);
            const float x = left.x + (right.x - left.x) * t;
            poly.append(mapPoint(x, curveBezierValueAt(points, fade.handles, x)));
        }
        poly.append(mapPoint(right.x, right.value));
    }
    p.setPen(QPen(color, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(poly);
}

}

TransitionFadeButton::TransitionFadeButton(QWidget* parent)
    : QToolButton(parent)
{
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setAutoRaise(true);
    setCursor(Qt::PointingHandCursor);
    QtControlSkin::setSemanticProperty(this, "fieldRole", QStringLiteral("preview"));
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged,
            this, [this]() { update(); });
    refresh();
}

void TransitionFadeButton::setFade(const TransitionFadeDef& fade)
{
    fade_ = fade;
    refresh();
}

void TransitionFadeButton::setDimmed(bool dimmed)
{
    if (dimmed_ == dimmed)
        return;
    dimmed_ = dimmed;
    refresh();
}

void TransitionFadeButton::refresh()
{
    QtControlSkin::setSemanticProperty(this, "dimmed", dimmed_);
    setToolTip(tr("%1 s").arg(formatSeconds(fade_.durationSeconds)));
    update();
}

QSize TransitionFadeButton::sizeHint() const
{
    return QToolButton::sizeHint().expandedTo(QSize(kThumbWidth + 6, 0));
}

void TransitionFadeButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    QtControlSkin::paintFieldFrame(painter, this);
    const auto& colors = ThemeManager::instance().colors();
    const QColor line = dimmed_ || !isEnabled() ? colors.textDisabled : colors.accent;
    paintFadeCurve(painter, QRectF(rect()).adjusted(3.0, 3.0, -3.0, -3.0), fade_, line);
}

TransitionFadePopup::TransitionFadePopup(const QString& title,
                                         const TransitionFadePopupContext& context,
                                         const TransitionFadeDef& fade,
                                         QWidget* parent,
                                         std::function<void(const TransitionFadeDef&)> apply,
                                         std::function<TransitionFadeDef()> load)
    : QWidget(parent, Qt::Popup)
    , apply_(std::move(apply))
    , load_(std::move(load))
    , duration_(fade.durationSeconds)
    , normalizedCurve_(fade.curve)
    , normalizedHandles_(fade.handles)
{
    setObjectName(QStringLiteral("transitionFadePopup"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_StyledBackground);
    resize(kPopupSize);

    QtControlSkin::setSemanticProperty(this, "surfaceRole", QStringLiteral("secondary"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->setSpacing(6);

    auto* titleRow = new QHBoxLayout();
    titleRow->setSpacing(6);
    auto* caption = new QLabel(title, this);
    caption->setObjectName(QStringLiteral("transitionFadeTitle"));
    QtControlSkin::setSemanticProperty(caption, "textRole", QStringLiteral("formLabel"));
    titleRow->addWidget(caption);

    auto* separator = new QLabel(QString(QChar(0x00B7)), this);
    QtControlSkin::setSemanticProperty(separator, "textRole", QStringLiteral("muted"));
    titleRow->addWidget(separator);

    auto* iconLabel = new QLabel(this);
    iconLabel->setObjectName(QStringLiteral("transitionFadeObjectIcon"));
    iconLabel->setFixedSize(16, 16);
    iconLabel->setScaledContents(true);
    if (context.objectIcon.isNull()) {
        iconLabel->hide();
    } else {
        iconLabel->setPixmap(context.objectIcon.pixmap(16, 16));
    }
    titleRow->addWidget(iconLabel);

    auto* objectLabel = new QLabel(context.objectName, this);
    objectLabel->setObjectName(QStringLiteral("transitionFadeObjectName"));
    QtControlSkin::setSemanticProperty(objectLabel, "textRole",
        context.dimmed ? QStringLiteral("secondary") : QStringLiteral("formLabel"));
    titleRow->addWidget(objectLabel, 1);
    layout->addLayout(titleRow);

    canvas_ = new CurveCanvas(this);
    CurveCanvasProfile profile;
    profile.axisScale = CurveAxisScale::LinearValue;
    profile.yMin = 0.0f;
    profile.yMax = 1.0f;
    profile.defaultEndValue = 1.0f;
    profile.colorRole = CurveColorRole::GridFade;
    profile.yAxisLabel = QObject::tr("Weight");
    profile.xAxisLabel = QObject::tr("Time (s)");
    profile.xMaxEditable = true;
    profile.xMaxMinimum = 0.0f;
    profile.xMaxMaximum = 3600.0f;
    profile.xDecimals = 4;
    profile.valueUnit = QString();
    profile.clipboardFamily = QStringLiteral("transitionfade");
    canvas_->setProfile(profile);
    layout->addWidget(canvas_, 1);

    resetButton_ = new QToolButton(this);
    resetButton_->setObjectName(QStringLiteral("transitionFadeReset"));
    resetButton_->setToolTip(QObject::tr("Reset"));
    resetButton_->setFixedSize(22, 22);
    resetButton_->setIconSize(QSize(16, 16));
    resetButton_->setAutoRaise(true);
    resetButton_->setCursor(Qt::PointingHandCursor);
    QtControlSkin::setSemanticProperty(resetButton_, "flatIconButton", true);
    titleRow->addWidget(resetButton_);

    refreshCanvas();
    refreshResetState();

    connect(canvas_, &CurveCanvas::curveEdited, this,
            [this](const QVector<CurvePointDef>& points,
                   const QVector<CurveHandlePairDef>& handles, const QString&) {
        const float scale = duration_ > 0.0f ? duration_ : 1.0f;
        normalizedCurve_.clear();
        for (const CurvePointDef& point : points)
            normalizedCurve_.push_back({point.x / scale, point.value});
        normalizedHandles_.clear();
        for (const CurveHandlePairDef& handle : handles) {
            normalizedHandles_.push_back({handle.h1x / scale, handle.h1y,
                                          handle.h2x / scale, handle.h2y});
        }
        curveNormalize(0.0f, 1.0f, normalizedCurve_, normalizedHandles_);
        refreshResetState();
        commit();
    });
    connect(canvas_, &CurveCanvas::xMaxEdited, this, [this](float next) {
        if (next == duration_)
            return;
        duration_ = next;
        refreshCanvas();
        refreshResetState();
        commit();
    });
    connect(resetButton_, &QToolButton::clicked, this, [this]() {
        resetToCanonical();
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this]() { style()->unpolish(this); style()->polish(this); });
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, this,
            [this]() { refreshResetState(); update(); });
    if (load_) {
        connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged, this,
                [this](ProjectItem*, const QString& property, const QObject*) {
            if (property == QStringLiteral("transitions"))
                reloadFromModel();
        }, Qt::QueuedConnection);
    }
}

QRect TransitionFadePopup::fittedFrame(const QPoint& globalPos, const QSize& size, const QRect& available)
{
    QRect frame(globalPos, size);
    if (!available.isValid())
        return frame;
    if (frame.right() > available.right())
        frame.moveRight(available.right());
    if (frame.bottom() > available.bottom())
        frame.moveBottom(available.bottom());
    if (frame.left() < available.left())
        frame.moveLeft(available.left());
    if (frame.top() < available.top())
        frame.moveTop(available.top());
    return frame;
}

void TransitionFadePopup::showAt(const QPoint& globalPos)
{
    QScreen* screen = QGuiApplication::screenAt(globalPos);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect();
    setGeometry(fittedFrame(globalPos, size(), available));
    show();
}

TransitionFadeDef TransitionFadePopup::currentFade() const
{
    TransitionFadeDef fade;
    fade.durationSeconds = duration_;
    fade.curve = normalizedCurve_;
    fade.handles = normalizedHandles_;
    return fade;
}

void TransitionFadePopup::loadFade(const TransitionFadeDef& fade)
{
    duration_ = fade.durationSeconds;
    normalizedCurve_ = fade.curve;
    normalizedHandles_ = fade.handles;
    refreshCanvas();
    refreshResetState();
}

void TransitionFadePopup::refreshCanvas()
{
    const float scale = duration_ > 0.0f ? duration_ : 1.0f;
    canvas_->setXDomain(0.0f, scale);
    CurveCanvasProfile profile = canvas_->profile();
    profile.xMaxEditValue = duration_;
    canvas_->setProfile(profile);
    QVector<CurvePointDef> scaledPoints;
    for (const CurvePointDef& point : normalizedCurve_)
        scaledPoints.push_back({point.x * scale, point.value});
    QVector<CurveHandlePairDef> scaledHandles;
    for (const CurveHandlePairDef& handle : normalizedHandles_) {
        scaledHandles.push_back({handle.h1x * scale, handle.h1y,
                                 handle.h2x * scale, handle.h2y});
    }
    canvas_->setCurve(scaledPoints, scaledHandles);
}

void TransitionFadePopup::refreshResetState()
{
    auto& manager = ThemeManager::instance();
    const ThemeAssetVariant variant{resetButton_->iconSize(), devicePixelRatioF(), 0,
                                    manager.colors().textSecondary};
    resetButton_->setIcon(manager.resolveIcon(QString::fromLatin1(ThemeAssetIds::AuditionReset), variant).icon);
    const bool isDefault = currentFade() == canonicalTransitionFade();
    resetButton_->setEnabled(!isDefault);
    resetButton_->setToolTip(isDefault
        ? QObject::tr("Already the default fade.")
        : QObject::tr("Reset"));
}

void TransitionFadePopup::resetToCanonical()
{
    const TransitionFadeDef canonical = canonicalTransitionFade();
    if (currentFade() == canonical)
        return;
    loadFade(canonical);
    commit();
}

void TransitionFadePopup::commit()
{
    if (apply_)
        apply_(currentFade());
}

void TransitionFadePopup::reloadFromModel()
{
    if (!load_)
        return;
    loadFade(load_());
}

}
