#include "Platform/AclHardening.h"
#include "Platform/UserSession.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QString>
#include <QTemporaryDir>

#ifdef Q_OS_WIN

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <string>

namespace {

bool aceSidIs(ACCESS_ALLOWED_ACE* ace, WELL_KNOWN_SID_TYPE type)
{
    BYTE wellKnown[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(wellKnown);
    if (!CreateWellKnownSid(type, nullptr, wellKnown, &size))
        return false;
    return EqualSid(&ace->SidStart, wellKnown) != FALSE;
}

}

TEST(AclHardeningTest, AppliesProtectedHandoffDacl)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    QString error;
    ASSERT_TRUE(VNH::applyHandoffAcl(dir.path(), &error))
        << error.toStdString();

    const std::wstring nativePath =
        QDir::toNativeSeparators(dir.path()).toStdWString();
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ASSERT_EQ(GetNamedSecurityInfoW(nativePath.c_str(), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION, nullptr,
                                    nullptr, &dacl, nullptr, &descriptor),
              static_cast<DWORD>(ERROR_SUCCESS));
    ASSERT_NE(dacl, nullptr);
    EXPECT_EQ(dacl->AceCount, 4);

    bool sawUsers = false;
    bool sawCreatorOwner = false;
    bool sawAdmins = false;
    bool sawSystem = false;
    for (WORD i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER* header = nullptr;
        ASSERT_TRUE(GetAce(dacl, i, reinterpret_cast<LPVOID*>(&header)));
        ASSERT_EQ(header->AceType, ACCESS_ALLOWED_ACE_TYPE);
        auto* ace = reinterpret_cast<ACCESS_ALLOWED_ACE*>(header);
        if (aceSidIs(ace, WinBuiltinUsersSid)) {
            sawUsers = true;
            EXPECT_TRUE(ace->Mask & FILE_ADD_FILE);
            EXPECT_FALSE(ace->Mask & FILE_ADD_SUBDIRECTORY);
            EXPECT_FALSE(ace->Mask & FILE_DELETE_CHILD);
            EXPECT_FALSE(ace->Mask & WRITE_DAC);
            EXPECT_FALSE(ace->Mask & WRITE_OWNER);
            EXPECT_EQ(header->AceFlags &
                          (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE),
                      0);
        } else if (aceSidIs(ace, WinCreatorOwnerSid)) {
            sawCreatorOwner = true;
            EXPECT_TRUE(header->AceFlags & OBJECT_INHERIT_ACE);
            EXPECT_TRUE(header->AceFlags & INHERIT_ONLY_ACE);
            EXPECT_FALSE(header->AceFlags & CONTAINER_INHERIT_ACE);
            EXPECT_EQ(ace->Mask & FILE_ALL_ACCESS, FILE_ALL_ACCESS);
        } else if (aceSidIs(ace, WinBuiltinAdministratorsSid)) {
            sawAdmins = true;
            EXPECT_EQ(ace->Mask & FILE_ALL_ACCESS, FILE_ALL_ACCESS);
        } else if (aceSidIs(ace, WinLocalSystemSid)) {
            sawSystem = true;
            EXPECT_EQ(ace->Mask & FILE_ALL_ACCESS, FILE_ALL_ACCESS);
        }
    }
    EXPECT_TRUE(sawUsers);
    EXPECT_TRUE(sawCreatorOwner);
    EXPECT_TRUE(sawAdmins);
    EXPECT_TRUE(sawSystem);

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ASSERT_TRUE(GetSecurityDescriptorControl(descriptor, &control, &revision));
    EXPECT_TRUE(control & SE_DACL_PROTECTED);
    LocalFree(descriptor);
}

TEST(AclHardeningTest, RejectsMissingDirectory)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    EXPECT_FALSE(VNH::applyHandoffAcl(
        dir.filePath(QStringLiteral("missing")), &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(AclHardeningTest, ResultRecipientCanReadAndAcknowledgeWithoutAdministratorRights)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(VNH::applyHandoffAcl(dir.path(), &error));
    const auto path = dir.filePath("result");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("completed");
    file.close();
    ASSERT_FALSE(VNH::fileOwnerId(path).isEmpty());
    ASSERT_TRUE(VNH::grantFileReadAndDelete(path, VNH::currentUserId(), &error))
        << error.toStdString();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto nativePath = QDir::toNativeSeparators(path).toStdWString();
    ASSERT_EQ(GetNamedSecurityInfoW(nativePath.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &descriptor), DWORD(ERROR_SUCCESS));
    const auto freeDescriptor = qScopeGuard([&] { LocalFree(descriptor); });
    HANDLE token = nullptr;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &token));
    const auto closeToken = qScopeGuard([&] { CloseHandle(token); });
    BYTE adminSid[SECURITY_MAX_SID_SIZE];
    DWORD sidSize = sizeof(adminSid);
    ASSERT_TRUE(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &sidSize));
    SID_AND_ATTRIBUTES disabled{adminSid, 0};
    HANDLE restricted = nullptr;
    ASSERT_TRUE(CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, 1, &disabled,
                                     0, nullptr, 0, nullptr, &restricted));
    const auto closeRestricted = qScopeGuard([&] { CloseHandle(restricted); });
    HANDLE impersonation = nullptr;
    ASSERT_TRUE(DuplicateToken(restricted, SecurityImpersonation, &impersonation));
    const auto closeImpersonation = qScopeGuard([&] { CloseHandle(impersonation); });
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    for (const DWORD access : {DWORD(FILE_READ_DATA), DWORD(DELETE), DWORD(FILE_WRITE_DATA)}) {
        PRIVILEGE_SET privileges{};
        DWORD privilegeBytes = sizeof(privileges);
        DWORD granted = 0;
        BOOL allowed = FALSE;
        ASSERT_TRUE(AccessCheck(descriptor, impersonation, access, &mapping,
                                &privileges, &privilegeBytes, &granted, &allowed));
        EXPECT_EQ(allowed != FALSE, access != FILE_WRITE_DATA);
    }
}

#elif defined(Q_OS_MACOS)

#include <QFile>

#include <sys/acl.h>
#include <sys/stat.h>

TEST(AclHardeningTest, AppliesStickyWorldWritableMode)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    QString error;
    ASSERT_TRUE(VNH::applyHandoffAcl(dir.path(), &error))
        << error.toStdString();

    struct stat info = {};
    ASSERT_EQ(stat(QFile::encodeName(dir.path()).constData(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, static_cast<mode_t>(S_ISVTX | 0777));
}

TEST(AclHardeningTest, DeniesSubdirectoryCreationForEveryone)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    QString error;
    ASSERT_TRUE(VNH::applyHandoffAcl(dir.path(), &error))
        << error.toStdString();

    acl_t acl = acl_get_file(QFile::encodeName(dir.path()).constData(),
                             ACL_TYPE_EXTENDED);
    ASSERT_NE(acl, nullptr);

    bool sawSubdirectoryDeny = false;
    bool sawAdminAllow = false;
    acl_entry_t entry = nullptr;
    int which = ACL_FIRST_ENTRY;
    while (acl_get_entry(acl, which, &entry) == 0) {
        which = ACL_NEXT_ENTRY;
        acl_tag_t tag = ACL_UNDEFINED_TAG;
        ASSERT_EQ(acl_get_tag_type(entry, &tag), 0);
        acl_permset_t permset = nullptr;
        ASSERT_EQ(acl_get_permset(entry, &permset), 0);
        if (tag == ACL_EXTENDED_DENY &&
            acl_get_perm_np(permset, ACL_ADD_SUBDIRECTORY) == 1)
            sawSubdirectoryDeny = true;
        if (tag == ACL_EXTENDED_ALLOW &&
            acl_get_perm_np(permset, ACL_WRITE_SECURITY) == 1)
            sawAdminAllow = true;
    }
    acl_free(acl);

    EXPECT_TRUE(sawSubdirectoryDeny);
    EXPECT_TRUE(sawAdminAllow);
}

TEST(AclHardeningTest, RejectsMissingDirectory)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    EXPECT_FALSE(VNH::applyHandoffAcl(
        dir.filePath(QStringLiteral("missing")), &error));
    EXPECT_FALSE(error.isEmpty());
}

#endif

TEST(AclHardeningTest, SddlDeniesSubdirectoriesForUsers)
{
    const QString sddl = VNH::handoffAclSddl();
    EXPECT_TRUE(sddl.startsWith(QStringLiteral("D:P")));
    EXPECT_TRUE(sddl.contains(QStringLiteral("(A;;0x1200ab;;;BU)")));
    EXPECT_TRUE(sddl.contains(QStringLiteral("(A;OIIO;FA;;;CO)")));
    EXPECT_TRUE(sddl.contains(QStringLiteral("(A;OICI;FA;;;BA)")));
    EXPECT_TRUE(sddl.contains(QStringLiteral("(A;OICI;FA;;;SY)")));
}
