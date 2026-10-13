#include "PlatformBar.h"

#include "../../Core/LanguageContextService.h"
#include "../../Core/PlatformContextService.h"
#include "../../Core/ProjectManager.h"
#include "../../Core/QtControlSkin.h"
#include "../Common/SegmentedControl.h"

#include <QAction>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <QWidget>

namespace VNX {

namespace {

class ToolBarButton final : public QToolButton
{
public:
    explicit ToolBarButton(QWidget* parent)
        : QToolButton(parent)
    {
        setFixedSize(22, 22);
        setProperty("flatIconButton", true);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStyleOptionToolButton option;
        initStyleOption(&option);
        QStylePainter painter(this);
        option.icon = QtControlSkin::current().toolBarIcon(option, painter.device()->devicePixelRatioF());
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
    }
};

}

PlatformBar::PlatformBar(QWidget* parent)
    : PlatformBar(PlatformContextService::instance(), parent)
{
}

PlatformBar::PlatformBar(PlatformContextService& service, QWidget* parent)
    : QToolBar(parent)
    , service_(&service)
{
    setObjectName(QStringLiteral("platformBar"));
    setMovable(false);
    setFloatable(false);
    setContextMenuPolicy(Qt::PreventContextMenu);
    setIconSize(QSize(20, 20));
    ensurePolished();

    host_ = new QWidget(this);
    host_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QGridLayout(host_);
    layout->setContentsMargins(4, 1, 4, 0);
    layout->setSpacing(6);
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(2, 1);
    leading_ = new QWidget(host_);
    auto* leadingLayout = new QHBoxLayout(leading_);
    leadingLayout->setContentsMargins(0, 0, 0, 0);
    leadingLayout->setSpacing(layout->spacing());
    leading_->hide();
    layout->addWidget(leading_, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);

    auto* trailing = new QWidget(host_);
    auto* trailingLayout = new QHBoxLayout(trailing);
    trailingLayout->setContentsMargins(0, 0, 0, 0);
    trailingLayout->setSpacing(layout->spacing());
    layout->addWidget(trailing, 0, 2, Qt::AlignRight | Qt::AlignVCenter);

    languageControl_ = new SegmentedControl(trailing);
    languageControl_->setObjectName(QStringLiteral("platformBarLanguages"));
    languageControl_->setAdaptiveSizing(true);
    languageControl_->layout()->setSpacing(layout->spacing());
    trailingLayout->addWidget(languageControl_);

    platformCombo_ = new QComboBox(trailing);
    platformCombo_->setObjectName(QStringLiteral("platformBarSelector"));
    platformCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    platformCombo_->setMinimumContentsLength(12);
    platformCombo_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    trailingLayout->addWidget(platformCombo_);

    allButton_ = new QToolButton(trailing);
    allButton_->setObjectName(QStringLiteral("platformBarAll"));
    allButton_->setProperty("flatIconButton", true);
    allButton_->setText(QStringLiteral("ALL"));
    allButton_->setAccessibleName(tr("Edit All Platforms"));
    allButton_->setToolTip(tr("Edit All Platforms"));
    allButton_->setCheckable(true);
    allButton_->setAutoRaise(true);
    allButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    allButton_->setCursor(Qt::PointingHandCursor);
    allButton_->setFocusPolicy(Qt::NoFocus);
    trailingLayout->addWidget(allButton_);

    addWidget(host_);

    connect(platformCombo_, &QComboBox::currentIndexChanged, this, &PlatformBar::onPlatformChanged);
    connect(allButton_, &QToolButton::toggled, this, &PlatformBar::onAllToggled);

    auto& pm = ProjectManager::instance();
    connect(&pm, &ProjectManager::projectOpened, this,
            [this](ProjectData*) { rebuild(); });
    connect(&pm, &ProjectManager::projectClosed, this, &PlatformBar::rebuild);
    connect(&pm, &ProjectManager::targetPlatformsChanged, this,
            [this](QStringList) { rebuild(); });
    connect(&pm, &ProjectManager::projectLanguagesChanged, this,
            [this](QStringList) { rebuildLanguages(); });
    connect(service_, &PlatformContextService::contextChanged, this,
            [this](const PlatformContext& context) { applyContext(context); });
    connect(&LanguageContextService::instance(), &LanguageContextService::languageChanged, this,
            [this](const QString& language) { applyLanguage(language); });

    rebuild();
}

QToolButton* PlatformBar::addLeadingAction(QAction* action, const QString& objectName)
{
    auto* button = new ToolBarButton(leading_);
    button->setObjectName(objectName);
    button->setDefaultAction(action);
    button->setAccessibleName(action->text());
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    leading_->layout()->addWidget(button);
    leading_->show();
    setVisible(true);
    return button;
}

void PlatformBar::addCenterWidget(QWidget* widget)
{
    center_ = widget;
    auto* layout = qobject_cast<QGridLayout*>(host_->layout());
    layout->addWidget(widget, 0, 1, Qt::AlignCenter);
    setVisible(true);
}

void PlatformBar::rebuild()
{
    updating_ = true;

    platformCombo_->clear();
    if (const auto* project = ProjectManager::instance().currentProject()) {
        for (const auto& platform : project->platforms) {
            platformCombo_->addItem(platform.name, platform.id);
            platformCombo_->setItemData(platformCombo_->count() - 1,
                QStringLiteral("%1 (%2)").arg(platform.name,
                    ProjectPlatformSemantics::basePlatformName(platform.basePlatform)), Qt::ToolTipRole);
        }
    }

    updating_ = false;
    applyContext(service_->context());
    rebuildLanguages();
    setVisible(ProjectManager::instance().isProjectOpen() || center_ || leading_->layout()->count() != 0);
}

void PlatformBar::rebuildLanguages()
{
    updating_ = true;
    languages_.clear();
    QList<QPair<QString, QVariant>> options;
    if (ProjectManager::instance().isProjectOpen()) {
        languages_ = LanguageContextService::instance().availableLanguages();
        for (const QString& name : languages_)
            options.append({name, name});
    }
    languageControl_->setOptions(options, true);
    for (int i = 0; i < languages_.size(); ++i) {
        QToolButton* button = languageControl_->button(i);
        if (!button)
            continue;
        const QString language = languages_.at(i);
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QToolButton::toggled, this,
                [this, language](bool on) { onLanguageToggled(language, on); });
    }
    updating_ = false;
    applyLanguage(LanguageContextService::instance().language());
    languageControl_->setVisible(!languages_.isEmpty());
}

void PlatformBar::applyLanguage(const QString& language)
{
    updating_ = true;
    for (int i = 0; i < languages_.size(); ++i) {
        QToolButton* button = languageControl_->button(i);
        if (!button)
            continue;
        button->setChecked(languages_.at(i) == language);
    }
    updating_ = false;
}

void PlatformBar::applyContext(const PlatformContext& context)
{
    updating_ = true;
    const bool hasTargets = platformCombo_->count() > 0;
    const bool projectOpen = ProjectManager::instance().isProjectOpen();
    allButton_->setVisible(projectOpen);
    allButton_->setEnabled(hasTargets);
    allButton_->setChecked(context.all);
    platformCombo_->setVisible(hasTargets);
    platformCombo_->setCurrentIndex(platformCombo_->findData(context.primary));
    const QString role = context.all ? tr("Audition Platform") : tr("Edit and Audition Platform");
    platformCombo_->setAccessibleName(role);
    platformCombo_->setToolTip(QStringLiteral("%1\n%2").arg(
        role, platformCombo_->currentData(Qt::ToolTipRole).toString()));
    updating_ = false;
}

void PlatformBar::onAllToggled(bool on)
{
    if (updating_)
        return;
    if (platformCombo_->count() == 0)
        return;

    service_->setAll(on);
}

void PlatformBar::onPlatformChanged(int index)
{
    if (updating_ || index < 0)
        return;

    const ProjectPlatformId platform = platformCombo_->itemData(index).toUuid();
    service_->setSelected({platform}, platform, service_->context().all);
}

void PlatformBar::onLanguageToggled(const QString& language, bool on)
{
    if (updating_)
        return;
    if (!on)
        return;
    LanguageContextService::instance().setLanguage(language);
}

}
