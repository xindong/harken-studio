#pragma once

#include <QCoreApplication>
#include <QLocale>
#include <QString>

namespace VNH {

class HubLocalization
{
public:
    static QString systemPreference();
    static QString englishPreference();
    static QString simplifiedChinesePreference();
    static QString resolveLanguage(const QString& preference,
                                   const QLocale& systemLocale);
    static QString effectiveLanguage();
    static bool install(QCoreApplication* application);
};

}
