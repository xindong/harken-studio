#include "ProjectPlatformEditor.h"
#include "Core/ThemeLayout.h"

#include "Core/QtControlSkin.h"
#include "Widgets/Common/DialogFormLayout.h"

#include <QComboBox>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace VNX {

ProjectPlatformEditor::ProjectPlatformEditor(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("projectPlatformEditor");
    setAttribute(Qt::WA_StyledBackground, true);
    QtControlSkin::setSemanticProperty(this, "surfaceRole", QStringLiteral("panel"));
    auto* layout = new QVBoxLayout(this);
    ThemeLayout::margins(layout, ThemeLayout::Space::Panel);
    form_ = new DialogFormLayout();
    name_ = new QLineEdit(this);
    name_->setObjectName("projectPlatformName");
    form_->addRow(tr("Platform:"), name_);
    base_ = new QComboBox(this);
    base_->setObjectName("projectPlatformBase");
    base_->addItems({"Windows", "Android", "Linux", "macOS", "iOS"});
    form_->addRow(tr("Base platform:"), base_);
    layout->addLayout(form_);
    connect(name_, &QLineEdit::editingFinished, this, &ProjectPlatformEditor::finishNameEditing);
    connect(name_, &QLineEdit::textChanged, this, [this] {
        const auto error = nameError();
        name_->setToolTip(error);
        QtControlSkin::setSemanticProperty(name_, "inputInvalid", !error.isEmpty());
    });
    connect(base_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        const auto before = platform_;
        const auto nextBase = *ProjectPlatformSemantics::parseBasePlatform(text);
        if (readOnly_ || nextBase == platform_.basePlatform) return;
        platform_.basePlatform = nextBase;
        emit platformChanged(before);
    });
    layout->addStretch();
    setPlatform(platform_);
}

void ProjectPlatformEditor::addField(const QString& label, QWidget* field)
{
    field->setParent(this);
    form_->addRow(label, field);
}

void ProjectPlatformEditor::setPlatform(const ProjectPlatform& platform, const QStringList& otherNames)
{
    platform_ = platform;
    otherNames_ = otherNames;
    const QSignalBlocker nameBlocker(name_);
    const QSignalBlocker baseBlocker(base_);
    name_->setText(platform_.name);
    name_->setToolTip(QString());
    QtControlSkin::setSemanticProperty(name_, "inputInvalid", false);
    base_->setCurrentText(ProjectPlatformSemantics::basePlatformName(platform_.basePlatform));
    updateControls();
}

void ProjectPlatformEditor::setReadOnly(bool readOnly)
{
    readOnly_ = readOnly;
    updateControls();
}

bool ProjectPlatformEditor::hasPendingName() const
{
    return name_->isModified();
}

void ProjectPlatformEditor::finishNameEditing()
{
    if (!name_->isModified()) return;
    const auto before = platform_;
    if (!nameError().isEmpty() || readOnly_) {
        discardNameEditing();
        emit platformChanged(before);
        return;
    }
    platform_.name = name_->text();
    name_->setModified(false);
    emit platformChanged(before);
}

void ProjectPlatformEditor::discardNameEditing()
{
    const QSignalBlocker blocker(name_);
    name_->setText(platform_.name);
    name_->setToolTip(QString());
    QtControlSkin::setSemanticProperty(name_, "inputInvalid", false);
}

QString ProjectPlatformEditor::nameError() const
{
    QString error;
    if (!ProjectPlatformSemantics::validateName(name_->text(), &error)) return error;
    for (const auto& name : otherNames_)
        if (name.compare(name_->text(), Qt::CaseInsensitive) == 0)
            return tr("A platform with this name already exists.");
    return {};
}

void ProjectPlatformEditor::updateControls()
{
    name_->setReadOnly(readOnly_);
    base_->setEnabled(!readOnly_);
}

}
