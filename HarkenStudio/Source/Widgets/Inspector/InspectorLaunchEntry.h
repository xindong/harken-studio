
#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QToolButton;

namespace VNX {

class InspectorLaunchEntry : public QWidget
{
    Q_OBJECT

public:
    explicit InspectorLaunchEntry(const QString& slot, QWidget* parent = nullptr,
                                  const QString& displayName = QString());

    void setSummary(const QString& summary);
    void setLaunchEnabled(bool enabled, const QString& disabledReason = QString());

    QString slot() const { return slot_; }

signals:
    void launchRequested(const QString& slot);

private:
    QString      slot_;
    QString      placeholder_;
    QLabel*      label_  = nullptr;
    QToolButton* button_ = nullptr;
};

}
