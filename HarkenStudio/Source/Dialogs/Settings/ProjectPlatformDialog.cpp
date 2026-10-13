#include "ProjectPlatformDialog.h"

#include "Core/QtControlSkin.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ProportionalTreeWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include "Widgets/Common/DecimalSpinBox.h"
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QScrollArea>
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
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
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
    QtControlSkin::setSemanticProperty(buttons->button(QDialogButtonBox::Ok), "settingsRole", QStringLiteral("primary"));
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

ProjectPlatformDialog::ProjectPlatformDialog(const ProjectPlatform& platform, bool create, QWidget* parent)
    : BaseDialog(parent), platform_(platform), profile_(platform.profile)
{
    setObjectName("projectPlatformDialog");
    setWindowTitle(create ? tr("Create Platform") : tr("Edit Platform"));
    resize(440, 360);
    auto* root = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget(scroll);
    scroll->setWidget(page);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll, 1);
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* name = new QLineEdit(platform.name, this);
    name->setObjectName("projectPlatformName");
    form->addRow(tr("Name:"), name);
    auto* base = new QComboBox(this);
    base->setObjectName("projectPlatformBase");
    base->addItems({"Windows", "macOS", "Linux", "Android", "iOS"});
    base->setCurrentText(ProjectPlatformSemantics::basePlatformName(platform.basePlatform));
    form->addRow(tr("Base platform:"), base);
    layout->addLayout(form);
    auto* resample = new QCheckBox(tr("Resample"), this);
    resample->setObjectName("platformResample");
    resample->setChecked(profile_.performResample);
    layout->addWidget(resample);
    auto* content = new TableWidget(this);
    content->setEnabled(profile_.performResample);
    layout->addWidget(content);
    auto* policies = content->createItemView(QStringLiteral("platformSizePolicies"),
        {tr("Range"), tr("Sample rate")}, {500, 500});
    policies->setSelectionMode(QAbstractItemView::SingleSelection);
    auto* policyFooter = content->footer();
    auto* add = policyFooter->addButton();
    add->setObjectName("platformPolicyAdd");
    auto* remove = policyFooter->removeButton();
    remove->setObjectName("platformPolicyDelete");
    const auto updateButtons = [=, this]() {
        const int index = policies->indexOfTopLevelItem(policies->currentItem());
        remove->setEnabled(index >= 0 && policies->currentItem()->isSelected()
            && profile_.sizePolicies.at(index).maxBytes.has_value());
    };
    connect(policies, &QTreeWidget::itemSelectionChanged, this, updateButtons);
    const auto refresh = [=, this](int selected) {
        const QSignalBlocker blocker(policies);
        policies->clear();
        for (const auto& policy : profile_.sizePolicies) {
            const int unit = policy.maxBytes ? displayUnit(*policy.maxBytes) : 0;
            const auto range = policy.maxBytes
                ? QStringLiteral("\u2264 %1 %2").arg(sizeValue(*policy.maxBytes, unit), sizeUnits[unit])
                : QStringLiteral("< Infinite");
            new QTreeWidgetItem(policies, {range, tr("%1 Hz").arg(policy.config.targetSampleRate)});
        }
        policies->setCurrentItem(policies->topLevelItem(selected));
        updateButtons();
    };
    refresh(-1);
    connect(resample, &QCheckBox::toggled, this, [=, this](bool checked) {
        profile_.performResample = checked;
        content->setEnabled(checked);
    });
    const auto resetProfile = [=, this]() {
        content->clearSelection();
        profile_ = ProjectPlatformProfile::fromBasePlatform(platform_.basePlatform);
        resample->setChecked(profile_.performResample);
        refresh(-1);
    };
    connect(base, &QComboBox::currentTextChanged, this, [=, this](const QString& text) {
        const auto nextBase = *ProjectPlatformSemantics::parseBasePlatform(text);
        if (nextBase == platform_.basePlatform) return;
        if (QMessageBox::question(this, tr("Change Base Platform"),
            tr("Reset resampling settings to the %1 defaults?").arg(text),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
            const QSignalBlocker blocker(base);
            base->setCurrentText(ProjectPlatformSemantics::basePlatformName(platform_.basePlatform));
            return;
        }
        platform_.basePlatform = nextBase;
        resetProfile();
    });
    const auto storePolicy = [=, this](const SizePolicy& policy, int index) {
        if (index < 0) profile_.sizePolicies.push_back(policy);
        else profile_.sizePolicies[index] = policy;
        std::sort(profile_.sizePolicies.begin(), profile_.sizePolicies.end(), [](const auto& a, const auto& b) {
            return a.maxBytes && (!b.maxBytes || *a.maxBytes < *b.maxBytes);
        });
        const auto found = std::find_if(profile_.sizePolicies.begin(), profile_.sizePolicies.end(),
            [&](const auto& row) { return row.maxBytes == policy.maxBytes; });
        refresh(int(found - profile_.sizePolicies.begin()));
    };
    connect(add, &QToolButton::clicked, this, [=, this]() {
        const int index = policies->indexOfTopLevelItem(policies->currentItem());
        SizePolicy policy{0, profile_.sizePolicies.at(index < 0 ? 0 : index).config};
        if (editPolicy(policy, true, profile_.sizePolicies, -1, this)) storePolicy(policy, -1);
    });
    connect(policies, &QTreeWidget::itemDoubleClicked, this, [=, this]() {
        const int index = policies->indexOfTopLevelItem(policies->currentItem());
        if (index < 0) return;
        auto policy = profile_.sizePolicies.at(index);
        if (editPolicy(policy, false, profile_.sizePolicies, index, this)) storePolicy(policy, index);
    });
    connect(remove, &QToolButton::clicked, this, [=, this]() {
        const int index = policies->indexOfTopLevelItem(policies->currentItem());
        if (index < 0 || !profile_.sizePolicies.at(index).maxBytes) return;
        content->removeSelectedRow([=, this] {
            profile_.sizePolicies.erase(profile_.sizePolicies.begin() + index);
            refresh(-1);
            return true;
        });
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Reset | QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QtControlSkin::setSemanticProperty(buttons->button(QDialogButtonBox::Ok), "settingsRole", QStringLiteral("primary"));
    buttons->setObjectName("projectPlatformDialogButtons");
    buttons->button(QDialogButtonBox::Reset)->setObjectName("projectPlatformReset");
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, this, resetProfile);
    layout->addStretch();
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [=, this]() {
        QString error;
        if (!ProjectPlatformSemantics::validateName(name->text(), &error)
            || !validateSizePolicies(profile_.sizePolicies, &error)) {
            QMessageBox::warning(this, tr("Project Platform"), error);
            return;
        }
        platform_.name = name->text();
        platform_.profile = profile_;
        accept();
    });
}

}
