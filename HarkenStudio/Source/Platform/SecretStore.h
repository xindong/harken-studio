#pragma once

#include <QtGlobal>

#if !defined(Q_OS_WIN) && !defined(Q_OS_MAC)
#error "SecretStore requires an OS-backed credential vault (Windows Credential Manager or macOS Keychain). Ports to other platforms must not persist credentials — accept them via process environment or session input only."
#endif

#include <QString>

namespace Harken::Platform::SecretStore {

bool store(const QString& service, const QString& account, const QString& secret);

QString load(const QString& service, const QString& account);

bool erase(const QString& service, const QString& account);

}
