#pragma once

#include <QString>
#include <QWidget>

class QLabel;

namespace VNX {

class MonitorPlaceholderPage : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorPlaceholderPage(const QString& title,
                                    const QString& gapId,
                                    QWidget* parent = nullptr);

    QString titleText() const;
    QString gapId() const;
    QString messageText() const;
    int visibleRowCount() const;

private:
    QLabel* messageLabel_ = nullptr;
    QString title_;
    QString gapId_;
};

} // namespace VNX
