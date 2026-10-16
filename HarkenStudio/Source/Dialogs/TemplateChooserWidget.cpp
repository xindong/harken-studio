
#include "TemplateChooserWidget.h"
#include "Core/FontManager.h"
#include "Widgets/Common/DialogFormLayout.h"

#include <QButtonGroup>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointF>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

namespace VNX {

namespace {

QString pointPairText(const QPointF& p) {
    return QStringLiteral("(%1, %2)").arg(p.x()).arg(p.y());
}

constexpr int kCardsPerRow = 4;
constexpr int kCardSlotHeight  = 100;
constexpr int kCardsRowSpacing = 8;

void renderBusNode(QTreeWidgetItem* parent, const TemplateBusNode& n) {
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, n.name);
    for (const auto& c : n.children)
        renderBusNode(item, c);
}

}


TemplateChooserWidget::TemplateChooserWidget(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
    rebuildTemplateCards();

    if (auto* empty = templateGroup_->button(0))
        empty->setChecked(true);
    applySelection(0);
    notifyValidityChanged();
}


void TemplateChooserWidget::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(14);

    {
        auto* row = new DialogFormLayout();
        nameLabel_ = new QLabel(tr("Project Name:"), this);
        nameEdit_ = new QLineEdit(QStringLiteral("MyGame_Audio"), this);
        nameEdit_->setPlaceholderText(tr("Enter project name"));
        connect(nameEdit_, &QLineEdit::textChanged,
                this, &TemplateChooserWidget::onNameChanged);
        row->addRow(nameLabel_, nameEdit_);
        root->addLayout(row);
    }

    {
        auto* lbl = new QLabel(tr("Template"), this);
        lbl->setProperty("templateRole", QStringLiteral("sectionTitle"));
        root->addWidget(lbl);

        cardsContainer_ = new QWidget(this);
        cardsContainer_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        cardsGrid_ = new QGridLayout(cardsContainer_);
        cardsGrid_->setContentsMargins(0, 0, 0, 0);
        cardsGrid_->setHorizontalSpacing(kCardsRowSpacing);
        cardsGrid_->setVerticalSpacing(kCardsRowSpacing);

        templateGroup_ = new QButtonGroup(this);
        templateGroup_->setExclusive(true);
        connect(templateGroup_,
                QOverload<int>::of(&QButtonGroup::idClicked),
                this, &TemplateChooserWidget::onTemplateButtonClicked);

        plusCard_ = new QPushButton(QStringLiteral("+"), this);
        plusCard_->setObjectName(QStringLiteral("wizardCard"));
        plusCard_->setProperty("templateRole", QStringLiteral("card"));
        plusCard_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        plusCard_->setToolTip(tr("Import a custom template JSON"));
        plusCard_->setCheckable(false);
        connect(plusCard_, &QPushButton::clicked,
                this, &TemplateChooserWidget::onPlusCardClicked);

        root->addWidget(cardsContainer_);

        root->addStretch(1);
    }

    {
        previewStack_ = new QStackedWidget(this);
        previewStack_->setMinimumHeight(280);

        previewPage_ = new QWidget(previewStack_);
        auto* prevLayout = new QHBoxLayout(previewPage_);
        prevLayout->setContentsMargins(0, 0, 0, 0);
        prevLayout->setSpacing(8);

        auto makeColumn = [&](const QString& header) {
            auto* col = new QTreeWidget(previewPage_);
            col->setColumnCount(1);
            col->setHeaderLabel(header);
            col->header()->setSectionResizeMode(QHeaderView::Stretch);
            col->setUniformRowHeights(false);
            col->setRootIsDecorated(true);
            prevLayout->addWidget(col, 1);
            return col;
        };
        treeBus_    = makeColumn(tr("Bus"));
        treeAtten_  = makeColumn(tr("Attenuation"));

        previewStack_->addWidget(previewPage_);

        errorPage_ = new QWidget(previewStack_);
        auto* errLayout = new QVBoxLayout(errorPage_);
        errLayout->setContentsMargins(0, 0, 0, 0);
        errLayout->setSpacing(6);
        errorSummary_ = new QLabel(errorPage_);
        errorSummary_->setProperty("templateRole", QStringLiteral("error"));
        errorSummary_->setWordWrap(true);
        errorText_ = new QTextEdit(errorPage_);
        errorText_->setReadOnly(true);
        errorText_->setLineWrapMode(QTextEdit::WidgetWidth);
        errorText_->setFont(FontManager::instance().getFont(FontRole::Mono));
        errLayout->addWidget(errorSummary_);
        errLayout->addWidget(errorText_, 1);

        previewStack_->addWidget(errorPage_);

        root->addWidget(previewStack_, 100);
    }
}


void TemplateChooserWidget::setNameFieldVisible(bool visible) {
    nameFieldVisible_ = visible;
    if (nameLabel_) nameLabel_->setVisible(visible);
    if (nameEdit_)  nameEdit_->setVisible(visible);
    notifyValidityChanged();
}

bool TemplateChooserWidget::isNameFieldVisible() const {
    return nameFieldVisible_;
}

QString TemplateChooserWidget::projectName() const {
    return nameEdit_ ? nameEdit_->text().trimmed() : QString();
}

void TemplateChooserWidget::setProjectName(const QString& name) {
    if (nameEdit_) nameEdit_->setText(name);
}


QPushButton* TemplateChooserWidget::makeCard(const QString& text,
                                             const QString& tooltip) {
    auto* btn = new QPushButton(this);
    btn->setObjectName(QStringLiteral("wizardCard"));
    btn->setProperty("templateRole", QStringLiteral("card"));
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setCheckable(true);

    auto* layout = new QVBoxLayout(btn);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(2);

    auto* nameLbl = new QLabel(text, btn);
    nameLbl->setAlignment(Qt::AlignCenter);
    nameLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
    nameLbl->setProperty("templateRole", QStringLiteral("cardTitle"));
    layout->addWidget(nameLbl);

    if (!tooltip.isEmpty()) {
        auto* descLbl = new QLabel(tooltip, btn);
        descLbl->setAlignment(Qt::AlignCenter);
        descLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        descLbl->setWordWrap(true);
        descLbl->setProperty("templateRole", QStringLiteral("cardDescription"));
        layout->addWidget(descLbl, 1);
        btn->setToolTip(tooltip);
    } else {
        layout->addStretch(1);
    }

    return btn;
}

void TemplateChooserWidget::rebuildTemplateCards()
{
    if (plusCard_) {
        cardsGrid_->removeWidget(plusCard_);
        plusCard_->setParent(cardsContainer_);
    }

    const auto existingButtons = templateGroup_->buttons();
    for (auto* b : existingButtons) {
        templateGroup_->removeButton(b);
        b->deleteLater();
    }

    currentLocations_ = ProjectTemplateLoader::listAvailable();

    auto place = [this](QWidget* w, int idx) {
        const int row = idx / kCardsPerRow;
        const int col = idx % kCardsPerRow;
        cardsGrid_->addWidget(w, row, col);
    };

    int idx = 0;
    auto* emptyCard = makeCard(tr("Empty"),
                               tr("Create an empty project (no template)"));
    templateGroup_->addButton(emptyCard, 0);
    place(emptyCard, idx++);

    for (int i = 0; i < static_cast<int>(currentLocations_.size()); ++i) {
        const auto& loc = currentLocations_[static_cast<size_t>(i)];
        auto* card = makeCard(loc.displayName, loc.description);
        templateGroup_->addButton(card, i + 1);
        place(card, idx++);
    }

    if (plusCard_)
        place(plusCard_, idx++);

    for (int c = 0; c < kCardsPerRow; ++c)
        cardsGrid_->setColumnStretch(c, 1);

    const int rows = (idx + kCardsPerRow - 1) / kCardsPerRow;
    const int totalHeight = rows * kCardSlotHeight
                          + (rows > 0 ? (rows - 1) : 0) * kCardsRowSpacing;
    cardsContainer_->setFixedHeight(totalHeight);
}


void TemplateChooserWidget::onTemplateButtonClicked(int id)
{
    applySelection(id);
}

void TemplateChooserWidget::applySelection(int id)
{
    selectedCardId_   = id;
    errorPageVisible_ = false;

    if (id == 0) {
        clearTrees();
        hidePreviewArea();
        result_.templateId   = QString();
        result_.templateData = ProjectTemplateData{};
        emit templateChanged();
        notifyValidityChanged();
        return;
    }

    const int idx = id - 1;
    if (idx < 0 || idx >= static_cast<int>(currentLocations_.size())) {
        clearTrees();
        hidePreviewArea();
        notifyValidityChanged();
        return;
    }
    const auto& loc = currentLocations_[static_cast<size_t>(idx)];
    auto loaded = ProjectTemplateLoader::load(loc.id);
    if (!loaded) {
        ValidationResult vr;
        vr.valid = false;
        vr.summary = tr("Template '%1' failed to load.").arg(loc.displayName);
        showErrorPage(vr);
        emit templateChanged();
        return;
    }
    populateTrees(*loaded);
    showPreviewPage();
    result_.templateId   = loaded->id;
    result_.templateData = *loaded;
    emit templateChanged();
    notifyValidityChanged();
}

void TemplateChooserWidget::selectTemplateById(const QString& templateId)
{
    for (int i = 0; i < static_cast<int>(currentLocations_.size()); ++i) {
        if (currentLocations_[static_cast<size_t>(i)].id == templateId) {
            const int cardId = i + 1;
            if (auto* btn = templateGroup_->button(cardId))
                btn->setChecked(true);
            applySelection(cardId);
            return;
        }
    }
}


void TemplateChooserWidget::onPlusCardClicked()
{
    const QString src = QFileDialog::getOpenFileName(
        this, tr("Import Template JSON"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("Template JSON (*.json);;All Files (*)"));
    if (src.isEmpty()) return;

    ValidationResult vr;
    auto newId = ProjectTemplateLoader::importFromFile(src, vr);
    if (!newId) {
        showErrorPage(vr);
        return;
    }
    rebuildTemplateCards();
    selectTemplateById(*newId);
}


void TemplateChooserWidget::populateTrees(const ProjectTemplateData& tpl)
{
    clearTrees();

    {
        auto* root = new QTreeWidgetItem(treeBus_);
        root->setText(0, tpl.bus.name);
        for (const auto& c : tpl.bus.children)
            renderBusNode(root, c);
    }

    for (const auto& a : tpl.attenuations) {
        auto* gi = new QTreeWidgetItem(treeAtten_);
        gi->setText(0, a.name);
        auto* mx = new QTreeWidgetItem(gi);
        mx->setText(0, QStringLiteral("max: %1").arg(a.maxDistance));
        auto* vol = new QTreeWidgetItem(gi);
        vol->setText(0, QStringLiteral("volume (%1)").arg(a.volumeCurve));
        QStringList pts;
        for (const auto& p : a.volumePoints)
            pts << pointPairText(p);
        if (!pts.isEmpty()) {
            auto* ptsRow = new QTreeWidgetItem(vol);
            ptsRow->setText(0, QStringLiteral("points: %1").arg(pts.join(", ")));
        }
    }

    treeBus_->expandAll();
    treeAtten_->expandAll();
}

void TemplateChooserWidget::clearTrees() {
    treeBus_->clear();
    treeAtten_->clear();
}


void TemplateChooserWidget::showErrorPage(const ValidationResult& vr) {
    errorPageVisible_ = true;
    if (errorSummary_)
        errorSummary_->setText(vr.summary);
    if (errorText_)
        errorText_->setPlainText(vr.toUserText());
    previewStack_->setCurrentWidget(errorPage_);
    previewStack_->setVisible(true);
    notifyValidityChanged();
}

void TemplateChooserWidget::showPreviewPage() {
    previewStack_->setCurrentWidget(previewPage_);
    previewStack_->setVisible(true);
}

void TemplateChooserWidget::hidePreviewArea() {
    previewStack_->setVisible(false);
}


void TemplateChooserWidget::onNameChanged() {
    notifyValidityChanged();
}

bool TemplateChooserWidget::isValidForCreate() const {
    if (errorPageVisible_) return false;
    if (nameFieldVisible_ && projectName().isEmpty()) return false;
    return true;
}

void TemplateChooserWidget::notifyValidityChanged() {
    const bool now = isValidForCreate();
    if (now != lastValidity_) {
        lastValidity_ = now;
        emit validityChanged(now);
    }
}

TemplateChooserWidget::Result TemplateChooserWidget::result() const {
    Result r = result_;
    r.projectName = projectName();
    return r;
}

}
