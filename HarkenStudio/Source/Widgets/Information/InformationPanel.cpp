
#include "InformationPanel.h"

#include "Core/AppSettings.h"
#include "Core/ContextInfoService.h"
#include "Core/FontManager.h"
#include "Core/HelpCatalog.h"
#include "Core/ItemTypeCatalog.h"

#include <QFrame>
#include <QHideEvent>
#include <QLabel>
#include <QScrollArea>
#include <QShowEvent>
#include <QVBoxLayout>

namespace VNX {

namespace {

const QString kMiddotSeparator = QStringLiteral(" \u00b7 ");
const QString kLocateHref = QStringLiteral("locate");

QString objectNameMarkup(const ContextTopic& topic)
{
    const QString escaped = topic.objectName.toHtmlEscaped();
    if (topic.objectGuid.isNull())
    {
        return escaped;
    }
    return QStringLiteral("<a href=\"%1\">%2</a>").arg(kLocateHref, escaped);
}

}

InformationPanel::InformationPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("InformationPanel"));
    setupUI();

    connect(&AppSettings::instance(), &AppSettings::fontSizeChanged,
            this, [this](PanelId panelId, int size)
            {
                if (panelId == PanelId::Information)
                {
                    applyFont(size);
                }
            });
    applyFont(AppSettings::instance().panelFontSize(PanelId::Information));

    connect(&AppSettings::instance(), &AppSettings::languageChanged,
            this, [this](const QString&)
            {
                renderTopic(ContextInfoService::instance().currentTopic());
            });

    ContextInfoService& service = ContextInfoService::instance();
    service.setFreezeRoot(this);

    connect(&service, &ContextInfoService::topicChanged,
            this, &InformationPanel::renderTopic);

    renderTopic(service.currentTopic());
}

void InformationPanel::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    headerLabel_ = new QLabel(this);
    headerLabel_->setObjectName(QStringLiteral("informationHeaderLabel"));
    headerLabel_->setTextFormat(Qt::RichText);
    headerLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    headerLabel_->setWordWrap(true);
    connect(headerLabel_, &QLabel::linkActivated, this, [this](const QString&)
            {
                if (!currentGuid_.isNull())
                {
                    emit locateRequested(currentGuid_);
                }
            });
    mainLayout->addWidget(headerLabel_);

    separator_ = new QFrame(this);
    separator_->setObjectName(QStringLiteral("informationSeparator"));
    separator_->setFrameShape(QFrame::HLine);
    separator_->setFrameShadow(QFrame::Plain);
    separator_->setFixedHeight(1);
    mainLayout->addWidget(separator_);

    QScrollArea* scrollArea = new QScrollArea(this);
    scrollArea->setObjectName(QStringLiteral("informationScrollArea"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    bodyLabel_ = new QLabel(scrollArea);
    bodyLabel_->setObjectName(QStringLiteral("informationBodyLabel"));
    bodyLabel_->setTextFormat(Qt::PlainText);
    bodyLabel_->setWordWrap(true);
    bodyLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    bodyLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scrollArea->setWidget(bodyLabel_);

    mainLayout->addWidget(scrollArea, 1);
}

void InformationPanel::applyFont(int size)
{
    setFont(FontManager::instance().getFont(FontRole::Body, size));
}

void InformationPanel::renderTopic(const ContextTopic& topic)
{
    currentGuid_ = topic.objectGuid;

    if (topic.isNone())
    {
        headerLabel_->clear();
        headerLabel_->setVisible(false);
        separator_->setVisible(false);
        bodyLabel_->clear();
        return;
    }

    headerLabel_->setVisible(true);
    separator_->setVisible(true);

    headerLabel_->setToolTip(QString());

    switch (topic.kind)
    {
        case ContextTopicKind::Folder:
            headerLabel_->setText(topic.objectName.toHtmlEscaped() + kMiddotSeparator + tr("Folder"));
            headerLabel_->setToolTip(topic.folderPath);
            break;
        case ContextTopicKind::Object:
        {
            if (!ItemTypeCatalog::isUserFacingType(topic.objectType))
            {
                headerLabel_->clear();
                headerLabel_->setVisible(false);
                separator_->setVisible(false);
                bodyLabel_->clear();
                return;
            }
            const QString typeName =
                ItemTypeCatalog::displayName(topic.objectType).toHtmlEscaped();
            headerLabel_->setText(objectNameMarkup(topic) + kMiddotSeparator + typeName);
            break;
        }
        case ContextTopicKind::Field:
        {
            if (!ItemTypeCatalog::isUserFacingType(topic.objectType))
            {
                headerLabel_->clear();
                headerLabel_->setVisible(false);
                separator_->setVisible(false);
                bodyLabel_->clear();
                return;
            }
            const QString label =
                topic.fieldLabel.isEmpty() ? topic.field : topic.fieldLabel;
            const QString typeName =
                ItemTypeCatalog::displayName(topic.objectType).toHtmlEscaped();
            headerLabel_->setText(label.toHtmlEscaped() + kMiddotSeparator + typeName
                                  + QStringLiteral(" \"") + objectNameMarkup(topic)
                                  + QStringLiteral("\""));
            break;
        }
        case ContextTopicKind::Panel:
        {
            const HelpEntry* entry = HelpCatalog::entryFor(topic);
            const QString title = entry && !entry->title.isEmpty() ? entry->title : topic.panelKey;
            headerLabel_->setText(title.toHtmlEscaped());
            break;
        }
        case ContextTopicKind::None:
            break;
    }

    bodyLabel_->setText(HelpCatalog::bodyFor(topic));
}

void InformationPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    ContextInfoService::instance().setActive(true);
}

void InformationPanel::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    ContextInfoService::instance().setActive(false);
}

}
