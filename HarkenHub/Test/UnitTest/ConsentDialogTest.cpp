#include "Legal/ConsentStore.h"
#include "Settings/HubSettings.h"
#include "UI/ConsentDialog.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>

namespace {

class ConsentDialogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.path().toUtf8());
        qunsetenv("HARKEN_HUB_EULA_FILE");
    }

    void TearDown() override
    {
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_EULA_FILE");
    }

    QTemporaryDir dir;
};

TEST_F(ConsentDialogTest, BothLanguagesIncludePrivacyAndRenewOldConsent)
{
    for (const QString& language : {QStringLiteral("en"), QStringLiteral("zh_CN")}) {
        ASSERT_TRUE(VNH::HubSettings::setLanguagePreference(language));
        QTranslator translator;
        if (language == QStringLiteral("zh_CN")) {
            ASSERT_TRUE(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
            QCoreApplication::installTranslator(&translator);
        }
        for (const QString& id : {VNH::ConsentDialog::hubDocumentId(),
                                  VNH::ConsentDialog::studioDocumentId()}) {
            ASSERT_TRUE(VNH::ConsentStore::recordConsent(
                id, QStringLiteral("2026-09-30"), QStringLiteral("old-text"),
                QStringLiteral("0.1.0"), QDateTime::currentDateTimeUtc()));
            QStringList shownTexts;
            QTimer::singleShot(0, [&shownTexts]() {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                EXPECT_NE(dialog, nullptr);
                if (!dialog)
                    return;
                const auto texts = dialog->findChildren<QPlainTextEdit*>();
                EXPECT_EQ(texts.size(), 2);
                for (auto* text : texts) {
                    EXPECT_TRUE(text->isReadOnly());
                    shownTexts.append(text->toPlainText());
                }
                dialog->accept();
            });
            ASSERT_TRUE(VNH::ConsentDialog::requestConsent(nullptr, id));
            ASSERT_EQ(shownTexts.size(), 2);
            const QString suffix = language == QStringLiteral("en")
                                       ? QStringLiteral(".txt")
                                       : QStringLiteral(".zh-CN.txt");
            QFile agreement(QStringLiteral(":/Resources/legal/USER_AGREEMENT") + suffix);
            QFile privacy(QStringLiteral(":/Resources/legal/PRIVACY_POLICY") + suffix);
            ASSERT_TRUE(agreement.open(QIODevice::ReadOnly | QIODevice::Text));
            ASSERT_TRUE(privacy.open(QIODevice::ReadOnly | QIODevice::Text));
            const QByteArray agreementText = agreement.readAll();
            const QByteArray privacyText = privacy.readAll();
            EXPECT_EQ(shownTexts.at(0), QString::fromUtf8(agreementText).replace(QChar::Nbsp, QChar::Space));
            EXPECT_EQ(shownTexts.at(1), QString::fromUtf8(privacyText).replace(QChar::Nbsp, QChar::Space));
            const QByteArray content = agreementText + "\n\n" + privacyText;
            const QString hash = QString::fromLatin1(QCryptographicHash::hash(
                content, QCryptographicHash::Sha256).toHex());
            EXPECT_TRUE(VNH::ConsentStore::hasConsent(
                id, QStringLiteral("2026-09-30-r2"), hash));
            EXPECT_TRUE(VNH::ConsentDialog::requestConsent(nullptr, id));
            QFile consent(VNH::ConsentStore::filePath());
            ASSERT_TRUE(consent.open(QIODevice::ReadOnly));
            const QByteArray beforeViewing = consent.readAll();
            consent.close();
            QTimer::singleShot(0, [&shownTexts]() {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                EXPECT_NE(dialog, nullptr);
                if (!dialog)
                    return;
                const auto texts = dialog->findChildren<QPlainTextEdit*>();
                EXPECT_EQ(texts.size(), 2);
                if (texts.size() == 2) {
                    for (qsizetype i = 0; i < texts.size(); ++i) {
                        EXPECT_TRUE(texts.at(i)->isReadOnly());
                        EXPECT_EQ(texts.at(i)->toPlainText(), shownTexts.at(i));
                    }
                    auto* agreementScroll = texts.at(0)->verticalScrollBar();
                    auto* privacyScroll = texts.at(1)->verticalScrollBar();
                    EXPECT_GT(agreementScroll->maximum(), 0);
                    EXPECT_GT(privacyScroll->maximum(), 0);
                    agreementScroll->setValue(agreementScroll->maximum());
                    EXPECT_EQ(privacyScroll->value(), 0);
                    privacyScroll->setValue(privacyScroll->maximum());
                    EXPECT_EQ(agreementScroll->value(), agreementScroll->maximum());
                }
                dialog->accept();
            });
            VNH::ConsentDialog::showDocuments(nullptr);
            ASSERT_TRUE(consent.open(QIODevice::ReadOnly));
            EXPECT_EQ(consent.readAll(), beforeViewing);
        }
    }
}

TEST_F(ConsentDialogTest, DeclineDoesNotRecordConsent)
{
    ASSERT_TRUE(VNH::HubSettings::setLanguagePreference(QStringLiteral("en")));
    QTimer::singleShot(0, []() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        EXPECT_NE(dialog, nullptr);
        if (dialog) dialog->reject();
    });
    EXPECT_FALSE(VNH::ConsentDialog::requestConsent(
        nullptr, VNH::ConsentDialog::hubDocumentId()));
    EXPECT_FALSE(QFile::exists(VNH::ConsentStore::filePath()));
}

TEST_F(ConsentDialogTest, ViewingDocumentsDoesNotCreateConsent)
{
    QTimer::singleShot(0, []() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        EXPECT_NE(dialog, nullptr);
        if (dialog)
            dialog->accept();
    });
    VNH::ConsentDialog::showDocuments(nullptr);
    EXPECT_FALSE(QFile::exists(VNH::ConsentStore::filePath()));
}

TEST_F(ConsentDialogTest, OverrideDocumentKeepsItsConsentHashAndContent)
{
    const QByteArray content("Custom license agreement\n\nCustom terms.");
    QFile file(dir.filePath(QStringLiteral("eula.txt")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(content), content.size());
    file.close();
    qputenv("HARKEN_HUB_EULA_FILE", file.fileName().toUtf8());
    const auto inspect = [&content]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        EXPECT_NE(dialog, nullptr);
        if (!dialog)
            return;
        const auto texts = dialog->findChildren<QPlainTextEdit*>();
        EXPECT_EQ(texts.size(), 1);
        if (texts.size() == 1) {
            EXPECT_TRUE(texts.first()->isReadOnly());
            EXPECT_EQ(texts.first()->toPlainText(), QString::fromUtf8(content));
        }
        dialog->accept();
    };
    QTimer::singleShot(0, inspect);
    ASSERT_TRUE(VNH::ConsentDialog::requestConsent(nullptr, VNH::ConsentDialog::hubDocumentId()));
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    EXPECT_TRUE(VNH::ConsentStore::hasConsent(
        VNH::ConsentDialog::hubDocumentId(), VNH::ConsentDialog::documentVersion(), hash));
    QTimer::singleShot(0, inspect);
    VNH::ConsentDialog::showDocuments(nullptr);
}

}
