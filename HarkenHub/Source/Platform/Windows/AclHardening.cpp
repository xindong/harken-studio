#include "Platform/AclHardening.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <string>

namespace VNH {

namespace {

bool applyPathAcl(const QString& path, const QString& sddl, QString* error)
{
    const std::wstring sddlText = sddl.toStdWString();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddlText.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        *error = QStringLiteral("Cannot parse the directory ACL (error %1)")
                     .arg(GetLastError());
        return false;
    }

    PACL dacl = nullptr;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    if (!GetSecurityDescriptorDacl(descriptor, &daclPresent, &dacl,
                                   &daclDefaulted) ||
        !daclPresent || !dacl) {
        LocalFree(descriptor);
        *error = QStringLiteral("The directory ACL has no DACL");
        return false;
    }

    const std::wstring nativePath =
        QDir::toNativeSeparators(path).toStdWString();
    const DWORD result = SetNamedSecurityInfoW(
        const_cast<wchar_t*>(nativePath.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);
    if (result != ERROR_SUCCESS) {
        *error = QStringLiteral("Cannot apply the directory ACL (error %1)")
                     .arg(result);
        return false;
    }
    return true;
}

}

bool applyDirectoryAcl(const QString& directoryPath, const QString& sddl, QString* error)
{
    if (!QFileInfo(directoryPath).isDir()) {
        *error = QStringLiteral("Directory is missing: %1").arg(directoryPath);
        return false;
    }
    return applyPathAcl(directoryPath, sddl, error);
}

QString fileOwnerId(const QString& path)
{
    const auto nativePath = QDir::toNativeSeparators(path).toStdWString();
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(nativePath.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                             &owner, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS)
        return {};
    LPWSTR sid = nullptr;
    QString result;
    if (ConvertSidToStringSidW(owner, &sid)) {
        result = QString::fromWCharArray(sid);
        LocalFree(sid);
    }
    LocalFree(descriptor);
    return result;
}

bool grantFileReadAndDelete(const QString& path, const QString& userId, QString* error)
{
    static const QRegularExpression sidPattern(QStringLiteral("^S-[0-9]+(?:-[0-9]+)+$"));
    const QFileInfo info(path);
    if (!sidPattern.match(userId).hasMatch() || !info.isFile()
        || info.isSymbolicLink() || info.isJunction()) {
        *error = QStringLiteral("Invalid installation result recipient or file: %1").arg(path);
        return false;
    }
    return applyPathAcl(path, QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x130089;;;%1)")
                                 .arg(userId), error);
}

bool applyHandoffAcl(const QString& directoryPath, QString* error)
{
    return applyDirectoryAcl(directoryPath, handoffAclSddl(), error);
}

}
