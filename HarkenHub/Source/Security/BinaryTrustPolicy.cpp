#include "Security/BinaryTrustPolicy.h"

#include <QDirIterator>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#endif

namespace VNH {

namespace {

#ifdef Q_OS_WIN
bool verifyAuthenticode(const QString& path, QString* error)
{
    const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = nativePath.c_str();

    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policy, &trustData);
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trustData);
    if (status != ERROR_SUCCESS) {
        *error = QStringLiteral("Authenticode verification failed for %1 (0x%2)")
                     .arg(path)
                     .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}
#endif

}

bool BinaryTrustPolicy::installedTreeRequired()
{
#ifdef HARKEN_HUB_TEST_OVERRIDES
    return qEnvironmentVariable("HARKEN_HUB_REQUIRE_AUTHENTICODE") ==
           QStringLiteral("1");
#elif defined(Q_OS_MACOS)
    return qEnvironmentVariable("HARKEN_HUB_REQUIRE_AUTHENTICODE") ==
           QStringLiteral("1");
#else
    return false;
#endif
}

bool BinaryTrustPolicy::installerRequired()
{
#ifdef HARKEN_HUB_TEST_OVERRIDES
    return qEnvironmentVariable("HARKEN_HUB_REQUIRE_AUTHENTICODE") ==
           QStringLiteral("1");
#elif defined(Q_OS_WIN)
    return true;
#elif defined(Q_OS_MACOS)
    return qEnvironmentVariable("HARKEN_HUB_REQUIRE_AUTHENTICODE") ==
           QStringLiteral("1");
#else
    return false;
#endif
}

bool BinaryTrustPolicy::verifyInstalledTree(const QString& installRoot,
                                            QString* error)
{
#ifdef Q_OS_WIN
    bool foundBinary = false;
    QDirIterator iterator(installRoot, QDir::Files | QDir::NoSymLinks,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix != QStringLiteral("exe") && suffix != QStringLiteral("dll"))
            continue;
        foundBinary = true;
        if (!verifyAuthenticode(path, error))
            return false;
    }
    if (!foundBinary) {
        *error = QStringLiteral("Installed tree contains no signed binaries");
        return false;
    }
    return true;
#else
    Q_UNUSED(installRoot);
    *error = QStringLiteral("Binary trust verification is unavailable");
    return false;
#endif
}

bool BinaryTrustPolicy::verifyInstallerBinary(const QString& installerPath,
                                              QString* error)
{
#ifdef Q_OS_WIN
    return verifyAuthenticode(installerPath, error);
#else
    Q_UNUSED(installerPath);
    *error = QStringLiteral("Binary trust verification is unavailable");
    return false;
#endif
}

}
