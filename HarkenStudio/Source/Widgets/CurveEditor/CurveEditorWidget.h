#pragma once

#include "CurveCanvas.h"
#include "Core/CurveSelection.h"
#include "Core/ProjectTypes.h"

#include <QUuid>
#include <QWidget>
#include <QVector>

class QLabel;
class QStackedWidget;

namespace VNX {

class CurveEditorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CurveEditorWidget(QWidget* parent = nullptr);
    ~CurveEditorWidget() override;

    void bindSelection(const CurveSelection& selection);
    void clear();

    bool hasAttenuation() const { return selection_.hasAttenuation(); }
    bool hasBinding() const { return selection_.hasBinding(); }
    QUuid attenuationGuid() const { return attenuation_.guid; }
    QString attenuationName() const { return attenuation_.name; }
    AttenuationDimension dimension() const { return dimension_; }
    QUuid bindingHostGuid() const { return selection_.hostGuid; }
    harkenstudio::ObjectId bindingStableId() const { return selection_.bindingStableId; }
    BindingAddress bindingAddress() const { return selection_.address; }
    const CurveSelection& selection() const { return selection_; }

    CurveCanvas* canvas() const { return canvas_; }

public slots:
    void refreshFromProject();

private slots:
    void onCanvasCurvePreviewed(const QVector<CurvePointDef>& points,
                               const QVector<CurveHandlePairDef>& handles);
    void finishCurvePreview();
    void onCanvasMaxDistanceEdited(float value);
    void onCanvasCurveEdited(const QVector<CurvePointDef>& points,
                             const QVector<CurveHandlePairDef>& handles,
                             const QString& editLabel);
    void onOutputRangeCommitted(float low, float high);

private:
    void setupUi();
    void reloadView(bool preserveCanvasSelection);
    void reloadAttenuationView(bool preserveCanvasSelection);
    void reloadBindingView(bool preserveCanvasSelection);
    void showMessage(const QString& text);
    void refreshBindingPlayhead();
    void applyCanvasSelection(bool preserveCanvasSelection,
                              const QVector<int>& previousSelection,
                              int previousPrimarySelection);

    AttenuationDimensionDef* currentDimensionDef();
    const AttenuationDimensionDef* currentDimensionDef() const;
    bool executeCurveUpdate(const QVector<CurvePointDef>& points,
                            const QVector<CurveHandlePairDef>& handles,
                            const QString& undoText = {});
    bool executeBindingUpdate(const QVector<CurvePointDef>& displayPoints,
                              const QVector<CurveHandlePairDef>& displayHandles,
                              const QString& undoText);
    std::optional<ParameterBindingDef> bindingWithCurve(
        const QVector<CurvePointDef>& displayPoints,
        const QVector<CurveHandlePairDef>& displayHandles) const;

    AttenuationSnapshot attenuation_;
    CurveSelection selection_;
    AttenuationDimension dimension_ = AttenuationDimension::Volume;
    bool updating_ = false;
    QUuid previewId_;

    QStackedWidget* stack_ = nullptr;
    QLabel* messageLabel_ = nullptr;
    QWidget* content_ = nullptr;

    CurveCanvas* canvas_ = nullptr;
};

}
