#include "Monitor/MonitorPlaceholderPage.h"

#include <QLabel>
#include <QVBoxLayout>

namespace VNX {

MonitorPlaceholderPage::MonitorPlaceholderPage(const QString& title,
                                               const QString& gapId,
                                               QWidget* parent)
    : QWidget(parent)
    , title_(title)
    , gapId_(gapId)
{
    setObjectName(QStringLiteral("MonitorPlaceholderPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    messageLabel_ = new QLabel(this);
    messageLabel_->setObjectName(QStringLiteral("MonitorPlaceholderMessage"));
    messageLabel_->setWordWrap(true);
    messageLabel_->setText(QStringLiteral("%1 is not wired. Gap: %2.").arg(title_, gapId_));
    layout->addWidget(messageLabel_);
    layout->addStretch(1);
}

QString MonitorPlaceholderPage::titleText() const
{
    return title_;
}

QString MonitorPlaceholderPage::gapId() const
{
    return gapId_;
}

QString MonitorPlaceholderPage::messageText() const
{
    return messageLabel_ ? messageLabel_->text() : QString();
}

int MonitorPlaceholderPage::visibleRowCount() const
{
    return 0;
}

} // namespace VNX
