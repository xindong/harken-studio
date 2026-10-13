
#pragma once

#include "Core/PropertyDescriptor.h"

#include <QString>
#include <QVariant>
#include <QWidget>

#include <vector>

class QLineEdit;
class QToolButton;

namespace VNX {

// Segmented row whose segments are not all constants: one of them owns a
// numeric sub-range of the field's domain and edits in place (double-click, or
// its context menu). The control reports what the user did and is told what to
// show -- it never interprets the field, so the segment layout arrives from the
// schema and the owner decides what each segment means.
class ValueSegmentedControl final : public QWidget
{
    Q_OBJECT

public:
    explicit ValueSegmentedControl(QWidget* parent = nullptr);

    void setSegments(const std::vector<SegmentSpec>& segments);

    // Identity of the field being edited, as "<guid>/<field>". A numeric
    // segment remembers its last authored count per context for the session:
    // once the field parks on a fixed segment that count is in no field any
    // more, and falling back to the schema default would discard it.
    void setContext(const QString& context);

    void setValue(const QVariant& value);

    bool isEditing() const { return editing_; }

    void cancelNumericEdit();
    void commitNumericEdit();

    static void clearSessionMemory();

signals:
    void segmentPicked(const QVariant& value);
    void numericEditBegan();
    void numericPreviewed(const QVariant& value);
    void numericEditEnded(bool committed);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Segment
    {
        SegmentSpec spec;
        QToolButton* button = nullptr;
    };

    void onSegmentClicked(int index);
    void beginNumericEdit();
    void endNumericEdit(bool committed);

    int      numericIndex() const;
    bool     holdsNumeric(const QVariant& value) const;
    QVariant numericFallback() const;
    QVariant clampNumeric(const QVariant& value) const;
    void     rememberNumeric(const QVariant& value);
    void     refreshSegments();

    std::vector<Segment> segments_;
    QLineEdit* editor_ = nullptr;
    QString context_;
    QVariant value_;
    QVariant numericSelection_;
    bool editing_ = false;
};

}
