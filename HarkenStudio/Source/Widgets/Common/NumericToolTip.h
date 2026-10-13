#pragma once

#include <QDoubleSpinBox>
#include <QEvent>
#include <QSpinBox>

namespace VNX {

class NumericToolTip final : public QObject
{
public:
    NumericToolTip(QAbstractSpinBox* editor, const QString& unit)
        : QObject(editor), editor_(editor), unit_(unit.trimmed())
    {
        editor_->installEventFilter(this);
        if (auto* spin = qobject_cast<QDoubleSpinBox*>(editor))
            connect(spin, &QDoubleSpinBox::textChanged, this, [this] { refresh(); });
        else if (auto* spin = qobject_cast<QSpinBox*>(editor))
            connect(spin, &QSpinBox::textChanged, this, [this] { refresh(); });
        refresh();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::ToolTip) refresh();
        return QObject::eventFilter(watched, event);
    }

private:
    void refresh()
    {
        editor_->setToolTip(QStringLiteral("%1 %2").arg(editor_->text(), unit_).trimmed());
    }

    QAbstractSpinBox* editor_;
    QString unit_;
};

}
