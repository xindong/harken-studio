#include "Platform/AclHardening.h"

#include <QDir>
#include <QFileInfo>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <string>

namespace VNH {

bool applyDirectoryAcl(const QString& directoryPath, const QString& sddl,
                       QString* error)
{
    const QFileInfo info(directoryPath);
    if (!info.exists() || !info.isDir()) {
        *error = QStringLiteral("Directory is missing: %1")
                     .arg(directoryPath);
        return false;
    }

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
        QDir::toNativeSeparators(directoryPath).toStdWString();
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

bool applyHandoffAcl(const QString& directoryPath, QString* error)
{
    return applyDirectoryAcl(directoryPath, handoffAclSddl(), error);
}

}
