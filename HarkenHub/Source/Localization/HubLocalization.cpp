#include "Localization/HubLocalization.h"

#include "Settings/HubSettings.h"

#include <QCoreApplication>
#include <QTranslator>

namespace VNH {

namespace {

QTranslator* hubTranslator = nullptr;
QTranslator* qtTranslator = nullptr;

void removeTranslator(QCoreApplication* application,
                      QTranslator*& translator)
{
    if (!translator)
        return;
    application->removeTranslator(translator);
    delete translator;
    translator = nullptr;
}

}

QString HubLocalization::systemPreference()
{
    return QStringLiteral("system");
}

QString HubLocalization::englishPreference()
{
    return QStringLiteral("en");
}

QString HubLocalization::simplifiedChinesePreference()
{
    return QStringLiteral("zh_CN");
}

QString HubLocalization::resolveLanguage(const QString& preference,
                                         const QLocale& systemLocale)
{
    if (preference == englishPreference() ||
        preference == simplifiedChinesePreference())
        return preference;
    return systemLocale.language() == QLocale::Chinese
               ? simplifiedChinesePreference()
               : englishPreference();
}

QString HubLocalization::effectiveLanguage()
{
    return resolveLanguage(HubSettings::languagePreference(),
                           QLocale::system());
}

bool HubLocalization::install(QCoreApplication* application)
{
    if (!application)
        return false;

    const QString language = effectiveLanguage();
    if (language == englishPreference()) {
        removeTranslator(application, qtTranslator);
        removeTranslator(application, hubTranslator);
        return true;
    }

    auto* nextHubTranslator = new QTranslator(application);
    if (!nextHubTranslator->load(
            QStringLiteral(":/i18n/harken_hub_zh_CN.qm"))) {
        delete nextHubTranslator;
        return false;
    }

    auto* nextQtTranslator = new QTranslator(application);
    if (!nextQtTranslator->load(QStringLiteral(":/i18n/qtbase_zh_CN.qm"))) {
        delete nextQtTranslator;
        nextQtTranslator = nullptr;
    }

    removeTranslator(application, qtTranslator);
    removeTranslator(application, hubTranslator);
    hubTranslator = nextHubTranslator;
    qtTranslator = nextQtTranslator;
    application->installTranslator(hubTranslator);
    if (qtTranslator)
        application->installTranslator(qtTranslator);
    return true;
}

}
