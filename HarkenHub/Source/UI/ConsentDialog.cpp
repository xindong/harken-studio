#include "UI/ConsentDialog.h"
#include "UI/DialogLayout.h"
#include "UI/MessageDialog.h"

#include "Legal/ConsentStore.h"
#include "Localization/HubLocalization.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace VNH {

namespace {

QStringList consentDocuments()
{
    const QString overridePath =
        qEnvironmentVariable("HARKEN_HUB_EULA_FILE");
    if (!overridePath.isEmpty()) {
        QFile file(overridePath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            return {QString::fromUtf8(file.readAll())};
    }
    const QString suffix =
        HubLocalization::effectiveLanguage() ==
                HubLocalization::simplifiedChinesePreference()
            ? QStringLiteral(".zh-CN.txt")
            : QStringLiteral(".txt");
    QFile agreement(QStringLiteral(":/Resources/legal/USER_AGREEMENT") + suffix);
    QFile privacy(QStringLiteral(":/Resources/legal/PRIVACY_POLICY") + suffix);
    if (!agreement.open(QIODevice::ReadOnly | QIODevice::Text) ||
        !privacy.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return {QString::fromUtf8(agreement.readAll()),
            QString::fromUtf8(privacy.readAll())};
}

}

QString ConsentDialog::hubDocumentId()
{
    return QStringLiteral("hub-eula");
}

QString ConsentDialog::studioDocumentId()
{
    return QStringLiteral("studio-eula");
}

QString ConsentDialog::documentVersion()
{
    return QStringLiteral("2026-10-28");
}

ConsentDialog::ConsentDialog(const QString& documentId,
                             const QStringList& documentTexts, Mode mode,
                             QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Harken Hub"));
    setModal(true);
    configureDialog(this, QSize(960, 600), QSize(480, 320));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);

    auto* title = new QLabel(
        documentId == studioDocumentId()
            ? tr("HarkenStudio License Agreement & Privacy Policy")
            : tr("License Agreement & Privacy Policy"),
        this);
    title->setObjectName(QStringLiteral("pageTitle"));
    title->setWordWrap(true);
    layout->addWidget(title);
    layout->addSpacing(10);

    const QStringList headings{tr("License Agreement"), tr("Privacy Policy")};
    auto* documents = new QHBoxLayout();
    documents->setSpacing(16);
    for (qsizetype i = 0; i < documentTexts.size(); ++i) {
        auto* column = new QVBoxLayout();
        if (documentTexts.size() > 1) {
            auto* heading = new QLabel(headings.at(i), this);
            heading->setWordWrap(true);
            column->addWidget(heading);
        }
        auto* text = new QPlainTextEdit(this);
        text->setAccessibleName(headings.at(i));
        text->setReadOnly(true);
        text->setPlainText(documentTexts.at(i));
        text->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { background-color: #171C28; border: 1px solid "
            "#313848; border-radius: 0; padding: 8px; color: #CCCCCC; }"));
        column->addWidget(text, 1);
        documents->addLayout(column, 1);
    }
    layout->addLayout(documents, 1);
    layout->addSpacing(12);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    if (mode == Mode::RequestConsent) {
        auto* decline = new QPushButton(tr("Decline"), this);
        connect(decline, &QPushButton::clicked, this, &QDialog::reject);
        buttons->addWidget(decline);
        auto* agree = new QPushButton(tr("Agree"), this);
        agree->setDefault(true);
        connect(agree, &QPushButton::clicked, this, &QDialog::accept);
        buttons->addWidget(agree);
    } else {
        auto* close = new QPushButton(tr("Close"), this);
        close->setDefault(true);
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
        buttons->addWidget(close);
    }
    layout->addLayout(buttons);
}

bool ConsentDialog::requestConsent(QWidget* parent, const QString& documentId)
{
    const QStringList documentTexts = consentDocuments();
    const QString documentText = documentTexts.join(QStringLiteral("\n\n"));
    if (documentText.isEmpty()) {
        MessageDialog::warning(
            parent, tr("Harken Hub"),
            tr("The license agreement and privacy policy could not be loaded."));
        return false;
    }
    const QString documentSha256 = QString::fromLatin1(
        QCryptographicHash::hash(documentText.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
    if (ConsentStore::hasConsent(documentId, documentVersion(),
                                 documentSha256))
        return true;
    ConsentDialog dialog(documentId, documentTexts, Mode::RequestConsent, parent);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    if (!ConsentStore::recordConsent(documentId, documentVersion(),
                                     documentSha256,
                                     QCoreApplication::applicationVersion(),
                                     QDateTime::currentDateTimeUtc())) {
        MessageDialog::warning(
            parent, tr("Harken Hub"),
            tr("Your consent could not be saved to %1. Please check that the "
               "folder is writable and try again.")
                .arg(ConsentStore::filePath()));
        return false;
    }
    return true;
}

void ConsentDialog::showDocuments(QWidget* parent)
{
    const QStringList documentTexts = consentDocuments();
    if (documentTexts.join(QStringLiteral("\n\n")).isEmpty()) {
        MessageDialog::warning(
            parent, tr("Harken Hub"),
            tr("The license agreement and privacy policy could not be loaded."));
        return;
    }
    ConsentDialog dialog(hubDocumentId(), documentTexts, Mode::ViewDocuments, parent);
    dialog.exec();
}

}
