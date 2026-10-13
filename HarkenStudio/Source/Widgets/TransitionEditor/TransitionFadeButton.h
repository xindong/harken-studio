#pragma once

#include "Core/TransitionSemantics.h"

#include <QIcon>
#include <QToolButton>
#include <QWidget>

#include <functional>

namespace VNX {

class CurveCanvas;

class TransitionFadeButton final : public QToolButton
{
    Q_OBJECT

public:
    explicit TransitionFadeButton(QWidget* parent = nullptr);

    void setFade(const TransitionFadeDef& fade);
    void setDimmed(bool dimmed);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void refresh();

    TransitionFadeDef fade_;
    bool dimmed_ = false;
};

struct TransitionFadePopupContext
{
    QString objectName;
    QIcon objectIcon;
    bool dimmed = false;
};

class TransitionFadePopup final : public QWidget
{
    Q_OBJECT

public:
    TransitionFadePopup(const QString& title,
                        const TransitionFadePopupContext& context,
                        const TransitionFadeDef& fade,
                        QWidget* parent,
                        std::function<void(const TransitionFadeDef&)> apply,
                        std::function<TransitionFadeDef()> load = {});

    void showAt(const QPoint& globalPos);

    static QRect fittedFrame(const QPoint& globalPos, const QSize& size, const QRect& available);

private:
    TransitionFadeDef currentFade() const;
    void loadFade(const TransitionFadeDef& fade);
    void refreshCanvas();
    void refreshResetState();
    void resetToCanonical();
    void commit();
    void reloadFromModel();

    std::function<void(const TransitionFadeDef&)> apply_;
    std::function<TransitionFadeDef()> load_;
    float duration_ = 0.5f;
    QVector<CurvePointDef> normalizedCurve_;
    QVector<CurveHandlePairDef> normalizedHandles_;
    CurveCanvas* canvas_ = nullptr;
    QToolButton* resetButton_ = nullptr;
};

}
