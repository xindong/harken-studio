#include "UI/ReleaseNotesDialog.h"

#include "ReleaseNotes/ReleaseNotesCatalog.h"

#include <QFrame>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

namespace VNH {

namespace {

QString categoryTitle(const QString& category)
{
    if (category == QStringLiteral("added"))
        return QCoreApplication::translate("VNH::ReleaseNotesDialog", "Added");
    if (category == QStringLiteral("removed"))
        return QCoreApplication::translate("VNH::ReleaseNotesDialog", "Removed");
    return QCoreApplication::translate("VNH::ReleaseNotesDialog", "Improved");
}

QWidget* createChanges(const ReleaseNotesEntry& entry, QWidget* parent)
{
    auto* container = new QWidget(parent);
    container->setObjectName(QStringLiteral("releaseNotesChanges"));
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(16, 4, 12, 12);
    layout->setSpacing(7);
    for (const QString& category : {QStringLiteral("added"),
                                    QStringLiteral("improved"),
                                    QStringLiteral("removed")}) {
        bool categoryAdded = false;
        for (const ReleaseNotesChange& change : entry.changes) {
            if (change.category != category)
                continue;
            if (!categoryAdded) {
                auto* categoryLabel = new QLabel(categoryTitle(category), container);
                categoryLabel->setObjectName(
                    QStringLiteral("releaseNotesCategory_%1").arg(category));
                layout->addWidget(categoryLabel);
                categoryAdded = true;
            }
            auto* label = new QLabel(
                QStringLiteral("\u2022 %1").arg(
                    ReleaseNotesCatalog::localizedChange(change.text)),
                container);
            label->setObjectName(QStringLiteral("releaseNotesChange"));
            label->setWordWrap(true);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(label);
        }
    }
    auto* contributors = new QLabel(
        QCoreApplication::translate("VNH::ReleaseNotesDialog", "Contributors") +
            QStringLiteral(":  @") + entry.contributors.join(QStringLiteral("  @")),
        container);
    contributors->setObjectName(QStringLiteral("releaseNotesContributors"));
    contributors->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contributors->setWordWrap(true);
    layout->addSpacing(5);
    layout->addWidget(contributors);
    return container;
}

}

ReleaseNotesDialog::ReleaseNotesDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("About Hub (%1)")
                       .arg(QCoreApplication::applicationVersion()));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(720, 640);
    setMinimumSize(520, 420);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 14);
    root->setSpacing(12);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("releaseNotesScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 8, 0);
    contentLayout->setSpacing(6);

    QString error;
    const QList<ReleaseNotesEntry> entries = ReleaseNotesCatalog::load(&error);
    catalogLoaded_ = !entries.isEmpty();
    if (entries.isEmpty()) {
        auto* errorLabel =
            new QLabel(tr("The changelog could not be loaded."), content);
        errorLabel->setWordWrap(true);
        contentLayout->addWidget(errorLabel);
    } else {
        const ReleaseNotesEntry& current = entries.first();
        auto* currentVersion = new QLabel(
            tr("%1 (Current Latest Version)").arg(current.version), content);
        currentVersion->setObjectName(QStringLiteral("currentReleaseVersion"));
        contentLayout->addWidget(currentVersion);
        contentLayout->addWidget(createChanges(current, content));

        auto* separator = new QFrame(content);
        separator->setObjectName(QStringLiteral("releaseNotesSeparator"));
        separator->setFrameShape(QFrame::HLine);
        contentLayout->addWidget(separator);

        for (qsizetype i = 1; i < entries.size(); ++i) {
            const ReleaseNotesEntry& entry = entries.at(i);
            auto* toggle = new QToolButton(content);
            toggle->setObjectName(QStringLiteral("releaseNotesVersion"));
            toggle->setText(entry.version);
            toggle->setCheckable(true);
            toggle->setChecked(false);
            toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            toggle->setArrowType(Qt::RightArrow);
            toggle->setSizePolicy(QSizePolicy::Expanding,
                                  QSizePolicy::Fixed);
            contentLayout->addWidget(toggle);

            QWidget* changes = createChanges(entry, content);
            changes->hide();
            contentLayout->addWidget(changes);
            connect(toggle, &QToolButton::toggled, changes,
                    [toggle, changes](bool expanded) {
                        toggle->setArrowType(expanded ? Qt::DownArrow
                                                      : Qt::RightArrow);
                        changes->setVisible(expanded);
                    });
        }
    }
    contentLayout->addStretch();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    auto* closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    buttonRow->addWidget(closeButton);
    root->addLayout(buttonRow);
}

bool ReleaseNotesDialog::hasLoadedCatalog() const
{
    return catalogLoaded_;
}

}
