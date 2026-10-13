#include "SecretStore.h"

#include <string>

#include <windows.h>
#include <wincred.h>

#pragma comment(lib, "advapi32.lib")

namespace {

std::wstring makeTargetW(const QString& service, const QString& account)
{
    return (service + QLatin1Char('/') + account).toStdWString();
}

}

namespace Harken::Platform::SecretStore {

bool store(const QString& service, const QString& account, const QString& secret)
{
    if (secret.isEmpty())
        return erase(service, account);

    const std::wstring targetW  = makeTargetW(service, account);
    const std::wstring accountW = account.toStdWString();
    const std::wstring secretW  = secret.toStdWString();

    CREDENTIALW cred = {};
    cred.Type              = CRED_TYPE_GENERIC;
    cred.TargetName        = const_cast<LPWSTR>(targetW.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(secretW.size() * sizeof(wchar_t));
    cred.CredentialBlob    = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(secretW.c_str()));
    cred.Persist           = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName          = const_cast<LPWSTR>(accountW.c_str());

    return CredWriteW(&cred, 0) != FALSE;
}

QString load(const QString& service, const QString& account)
{
    const std::wstring targetW = makeTargetW(service, account);

    PCREDENTIALW cred = nullptr;
    if (!CredReadW(targetW.c_str(), CRED_TYPE_GENERIC, 0, &cred))
        return {};

    QString secret;
    if (cred && cred->CredentialBlob && cred->CredentialBlobSize > 0)
    {
        secret = QString::fromWCharArray(
            reinterpret_cast<const wchar_t*>(cred->CredentialBlob),
            cred->CredentialBlobSize / sizeof(wchar_t));
    }

    if (cred)
        CredFree(cred);

    return secret;
}

bool erase(const QString& service, const QString& account)
{
    const std::wstring targetW = makeTargetW(service, account);
    if (CredDeleteW(targetW.c_str(), CRED_TYPE_GENERIC, 0))
        return true;
    return GetLastError() == ERROR_NOT_FOUND;
}

}
