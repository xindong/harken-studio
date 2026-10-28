#include "ProjectPlatformEditor.h"

#include "Core/BaseDialog.h"
#include "Core/QtControlSkin.h"
#include "Widgets/Common/DialogFormLayout.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ProportionalTreeWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include "Widgets/Common/DecimalSpinBox.h"
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeWidget>

#include <algorithm>
#include <limits>

namespace VNX {
namespace {

const QStringList sizeUnits{"B", "KB", "MB", "GB"};

uint64_t unitScale(int unit)
{
    return uint64_t(1) << (10 * unit);
}

QString sizeValue(uint64_t bytes, int unit)
{
    const auto scale = unitScale(unit);
    QString value = QString::number(bytes / scale);
    auto remainder = bytes % scale;
    if (remainder) value += QChar('.');
    while (remainder) {
        remainder *= 10;
        value += QChar(char('0' + remainder / scale));
        remainder %= scale;
    }
    return value;
}

int displayUnit(uint64_t bytes)
{
    for (int unit = sizeUnits.size() - 1; unit > 0; --unit) {
        if (bytes < unitScale(unit)) continue;
        const auto value = sizeValue(bytes, unit);
        const auto decimal = value.indexOf(QChar('.'));
        if (decimal < 0 || value.size() - decimal - 1 <= 3) return unit;
    }
    return 0;
}

std::optional<uint64_t> sizeBytes(const QString& text, int unit)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9]+(?:\\.[0-9]+)?$"));
    if (!pattern.match(text).hasMatch()) return std::nullopt;
    const auto parts = text.split(QChar('.'));
    bool valid = false;
    const auto whole = parts.front().toULongLong(&valid);
    if (!valid) return std::nullopt;
    const auto scale = unitScale(unit);
    uint64_t fraction = 0;
    if (parts.size() == 2) {
        for (qsizetype i = parts.back().size(); i > 0; --i) {
            const auto product = uint64_t(parts.back()[i - 1].digitValue()) * scale + fraction;
            if (product % 10 != 0) return std::nullopt;
            fraction = product / 10;
        }
    }
    const auto maximum = uint64_t(std::numeric_limits<qint64>::max());
    if (whole > (maximum - fraction) / scale) return std::nullopt;
    return whole * scale + fraction;
}

bool editPolicy(SizePolicy& policy, bool create, const std::vector<SizePolicy>& policies,
                int index, QWidget* parent)
{
    BaseDialog dialog(parent);
    dialog.setWindowTitle(create ? QObject::tr("Add Range") : QObject::tr("Edit Range"));
    dialog.setObjectName("platformSizePolicyDialog");
    dialog.resize(340, 160);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new DialogFormLayout();
    auto* limit = new QWidget(&dialog);
    auto* limitLayout = new QHBoxLayout(limit);
    limitLayout->setContentsMargins(0, 0, 0, 0);
    QLineEdit* value = nullptr;
    QComboBox* unit = nullptr;
    int currentUnit = policy.maxBytes ? displayUnit(*policy.maxBytes) : 0;
    if (policy.maxBytes) {
        value = new QLineEdit(create ? QString() : sizeValue(*policy.maxBytes, currentUnit), limit);
        value->setObjectName("platformPolicyLimit");
        value->setMaxLength(64);
        value->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9]+(?:\\.[0-9]+)?")), value));
        unit = new QComboBox(limit);
        unit->setObjectName("platformPolicyUnit");
        unit->addItems(sizeUnits);
        unit->setCurrentIndex(currentUnit);
        limitLayout->addWidget(value, 1);
        limitLayout->addWidget(unit);
        QObject::connect(unit, &QComboBox::currentIndexChanged, &dialog, [&](int next) {
            const auto bytes = sizeBytes(value->text(), currentUnit);
            currentUnit = next;
            if (bytes) value->setText(sizeValue(*bytes, currentUnit));
        });
    } else {
        auto* infinite = new QLabel(QObject::tr("Infinite"), limit);
        infinite->setObjectName("platformPolicyInfinite");
        limitLayout->addWidget(infinite);
    }
    form->addRow(QObject::tr("Upper limit:"), limit);
    auto* rate = new DecimalSpinBox(&dialog);
    rate->setObjectName("platformSampleRate");
    rate->setDecimals(0);
    rate->setRange(1, UINT32_MAX);
    rate->setValue(policy.config.targetSampleRate);
    rate->setToolTip(QStringLiteral("Hz"));
    rate->setKeyboardTracking(false);
    form->addRow(QObject::tr("Sample rate:"), rate);
    layout->addLayout(form);
    auto* error = new QLabel(&dialog);
    error->setObjectName("platformPolicyError");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        auto candidate = policy;
        if (value) {
            candidate.maxBytes = sizeBytes(value->text(), currentUnit);
            if (!candidate.maxBytes) {
                error->setText(QObject::tr("Enter a whole-byte size within range."));
                error->show();
                return;
            }
        }
        for (size_t i = 0; i < policies.size(); ++i) {
            if (int(i) != index && policies[i].maxBytes == candidate.maxBytes) {
                error->setText(QObject::tr("This limit already exists."));
                error->show();
                return;
            }
        }
        candidate.config.targetSampleRate = uint32_t(rate->value());
        policy = candidate;
        dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}

}

ProjectPlatformEditor::ProjectPlatformEditor(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("projectPlatformEditor");
    setAttribute(Qt::WA_StyledBackground, true);
    QtControlSkin::setSemanticProperty(this, "surfaceRole", QStringLiteral("panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    auto* form = new DialogFormLayout();
    name_ = new QLineEdit(this);
    name_->setObjectName("projectPlatformName");
    form->addRow(tr("Platform:"), name_);
    base_ = new QComboBox(this);
    base_->setObjectName("projectPlatformBase");
    base_->addItems({"Windows", "Android", "Linux", "macOS", "iOS"});
    form->addRow(tr("Base platform:"), base_);
    layout->addLayout(form);
    resample_ = new QCheckBox(this);
    resample_->setObjectName("platformResample");
    form->addRow(tr("Resample"), resample_);
    content_ = new TableWidget(this);
    layout->addWidget(content_);
    policies_ = content_->createItemView(QStringLiteral("platformSizePolicies"),
        {tr("Range"), tr("Sample rate")}, {500, 500});
    policies_->setSelectionMode(QAbstractItemView::SingleSelection);
    auto* policyFooter = content_->footer();
    auto* add = policyFooter->addButton();
    add->setObjectName("platformPolicyAdd");
    auto* remove = policyFooter->removeButton();
    remove->setObjectName("platformPolicyDelete");
    connect(policies_, &QTreeWidget::itemSelectionChanged, this, &ProjectPlatformEditor::updateControls);
    connect(name_, &QLineEdit::editingFinished, this, &ProjectPlatformEditor::finishNameEditing);
    connect(name_, &QLineEdit::textChanged, this, [this] {
        const auto error = nameError();
        name_->setToolTip(error);
        QtControlSkin::setSemanticProperty(name_, "inputInvalid", !error.isEmpty());
    });
    connect(resample_, &QCheckBox::toggled, this, [this](bool checked) {
        const auto before = platform_;
        platform_.profile.performResample = checked;
        updateControls();
        emit platformChanged(before);
    });
    connect(base_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        const auto before = platform_;
        const auto nextBase = *ProjectPlatformSemantics::parseBasePlatform(text);
        if (nextBase == platform_.basePlatform) return;
        if (QMessageBox::question(this, tr("Change Base Platform"),
            tr("Reset resampling settings to the %1 defaults?").arg(text),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
            const QSignalBlocker blocker(base_);
            base_->setCurrentText(ProjectPlatformSemantics::basePlatformName(platform_.basePlatform));
            return;
        }
        if (platform_.id != before.id || platform_.basePlatform != before.basePlatform || readOnly_) {
            const QSignalBlocker blocker(base_);
            base_->setCurrentText(ProjectPlatformSemantics::basePlatformName(platform_.basePlatform));
            return;
        }
        platform_.basePlatform = nextBase;
        platform_.profile = ProjectPlatformProfile::fromBasePlatform(nextBase);
        emit platformChanged(before);
    });
    connect(add, &QToolButton::clicked, this, [this]() {
        const auto before = platform_.toJson();
        const int index = policies_->indexOfTopLevelItem(policies_->currentItem());
        SizePolicy policy{0, platform_.profile.sizePolicies.at(index < 0 ? 0 : index).config};
        if (editPolicy(policy, true, platform_.profile.sizePolicies, -1, this)
            && !readOnly_ && platform_.toJson() == before)
            storePolicy(policy, -1);
    });
    connect(policies_, &QTreeWidget::itemDoubleClicked, this, [this]() {
        const int index = policies_->indexOfTopLevelItem(policies_->currentItem());
        if (readOnly_ || index < 0) return;
        const auto before = platform_.toJson();
        auto policy = platform_.profile.sizePolicies.at(index);
        if (editPolicy(policy, false, platform_.profile.sizePolicies, index, this)
            && !readOnly_ && platform_.toJson() == before)
            storePolicy(policy, index);
    });
    connect(remove, &QToolButton::clicked, this, [this]() {
        const int index = policies_->indexOfTopLevelItem(policies_->currentItem());
        if (index < 0 || !platform_.profile.sizePolicies.at(index).maxBytes) return;
        content_->removeSelectedRow([this, index] {
            const auto before = platform_;
            platform_.profile.sizePolicies.erase(platform_.profile.sizePolicies.begin() + index);
            refreshPolicies();
            emit platformChanged(before);
            return true;
        });
    });
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Reset, this);
    buttons_->setObjectName("projectPlatformDialogButtons");
    auto* reset = buttons_->button(QDialogButtonBox::Reset);
    reset->setObjectName("projectPlatformReset");
    reset->setAutoDefault(false);
    connect(reset, &QPushButton::clicked, this, &ProjectPlatformEditor::resetProfile);
    layout->addStretch();
    layout->addWidget(buttons_, 0, Qt::AlignRight);
    setPlatform(platform_);
}

void ProjectPlatformEditor::setPlatform(const ProjectPlatform& platform, const QStringList& otherNames)
{
    platform_ = platform;
    otherNames_ = otherNames;
    const QSignalBlocker nameBlocker(name_);
    const QSignalBlocker baseBlocker(base_);
    const QSignalBlocker resampleBlocker(resample_);
    name_->setText(platform_.name);
    name_->setToolTip(QString());
    QtControlSkin::setSemanticProperty(name_, "inputInvalid", false);
    base_->setCurrentText(ProjectPlatformSemantics::basePlatformName(platform_.basePlatform));
    resample_->setChecked(platform_.profile.performResample);
    refreshPolicies();
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

void ProjectPlatformEditor::refreshPolicies(int selected)
{
    const QSignalBlocker blocker(policies_);
    policies_->clear();
    for (const auto& policy : platform_.profile.sizePolicies) {
        const int unit = policy.maxBytes ? displayUnit(*policy.maxBytes) : 0;
        const auto range = policy.maxBytes
            ? QStringLiteral("\u2264 %1 %2").arg(sizeValue(*policy.maxBytes, unit), sizeUnits[unit])
            : QStringLiteral("< Infinite");
        new QTreeWidgetItem(policies_, {range, tr("%1 Hz").arg(policy.config.targetSampleRate)});
    }
    policies_->setCurrentItem(policies_->topLevelItem(selected));
    updateControls();
}

void ProjectPlatformEditor::updateControls()
{
    name_->setReadOnly(readOnly_);
    base_->setEnabled(!readOnly_);
    resample_->setEnabled(!readOnly_);
    content_->setEnabled(readOnly_ || platform_.profile.performResample);
    auto* footer = content_->footer();
    footer->setVisible(!readOnly_);
    footer->addButton()->setEnabled(!readOnly_);
    const int index = policies_->indexOfTopLevelItem(policies_->currentItem());
    footer->removeButton()->setEnabled(!readOnly_ && index >= 0 && policies_->currentItem()->isSelected()
        && platform_.profile.sizePolicies.at(index).maxBytes.has_value());
    buttons_->setVisible(!readOnly_);
}

void ProjectPlatformEditor::resetProfile()
{
    const auto before = platform_;
    platform_.profile = ProjectPlatformProfile::fromBasePlatform(platform_.basePlatform);
    const QSignalBlocker blocker(resample_);
    resample_->setChecked(platform_.profile.performResample);
    refreshPolicies();
    emit platformChanged(before);
}

void ProjectPlatformEditor::storePolicy(const SizePolicy& policy, int index)
{
    const auto before = platform_;
    auto& policies = platform_.profile.sizePolicies;
    if (index < 0) policies.push_back(policy);
    else policies[index] = policy;
    std::sort(policies.begin(), policies.end(), [](const auto& a, const auto& b) {
        return a.maxBytes && (!b.maxBytes || *a.maxBytes < *b.maxBytes);
    });
    const auto found = std::find_if(policies.begin(), policies.end(),
        [&](const auto& row) { return row.maxBytes == policy.maxBytes; });
    refreshPolicies(int(found - policies.begin()));
    emit platformChanged(before);
}

}
