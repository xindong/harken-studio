#include "Platform/AclHardening.h"

#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <grp.h>
#include <initializer_list>
#include <membership.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace VNH {

namespace {

bool resolveGroupUuid(const char* groupName, uuid_t uuid)
{
    struct group* entry = getgrnam(groupName);
    if (!entry)
        return false;
    return mbr_gid_to_uuid(entry->gr_gid, uuid) == 0;
}

bool appendEntry(acl_t& acl, acl_tag_t tag, const uuid_t qualifier,
                 std::initializer_list<acl_perm_t> permissions, bool inherit)
{
    acl_entry_t entry = nullptr;
    if (acl_create_entry(&acl, &entry) != 0)
        return false;
    if (acl_set_tag_type(entry, tag) != 0)
        return false;
    if (acl_set_qualifier(entry, qualifier) != 0)
        return false;
    acl_permset_t permset = nullptr;
    if (acl_get_permset(entry, &permset) != 0)
        return false;
    for (const acl_perm_t permission : permissions)
        if (acl_add_perm(permset, permission) != 0)
            return false;
    if (acl_set_permset(entry, permset) != 0)
        return false;
    if (inherit) {
        acl_flagset_t flags = nullptr;
        if (acl_get_flagset_np(entry, &flags) != 0)
            return false;
        if (acl_add_flag_np(flags, ACL_ENTRY_FILE_INHERIT) != 0 ||
            acl_add_flag_np(flags, ACL_ENTRY_DIRECTORY_INHERIT) != 0)
            return false;
        if (acl_set_flagset_np(entry, flags) != 0)
            return false;
    }
    return true;
}

}

bool applyHandoffAcl(const QString& directoryPath, QString* error)
{
    const QFileInfo info(directoryPath);
    if (!info.exists() || !info.isDir()) {
        *error = QStringLiteral("Handoff directory is missing: %1")
                     .arg(directoryPath);
        return false;
    }

    const QByteArray nativePath = QFile::encodeName(directoryPath);

    if (chmod(nativePath.constData(), S_ISVTX | 0777) != 0) {
        *error = QStringLiteral(
                     "Cannot set the handoff directory mode (errno %1)")
                     .arg(errno);
        return false;
    }

    if (geteuid() == 0) {
        struct group* admin = getgrnam("admin");
        if (!admin ||
            chown(nativePath.constData(), 0, admin->gr_gid) != 0) {
            *error = QStringLiteral(
                         "Cannot set the handoff directory ownership "
                         "(errno %1)")
                         .arg(errno);
            return false;
        }
    }

    uuid_t everyoneUuid;
    uuid_t adminUuid;
    if (!resolveGroupUuid("everyone", everyoneUuid) ||
        !resolveGroupUuid("admin", adminUuid)) {
        *error = QStringLiteral("Cannot resolve the handoff ACL groups");
        return false;
    }

    acl_t acl = acl_init(2);
    if (!acl) {
        *error = QStringLiteral("Cannot allocate the handoff ACL");
        return false;
    }

    const bool built =
        appendEntry(acl, ACL_EXTENDED_DENY, everyoneUuid,
                    {ACL_ADD_SUBDIRECTORY}, false) &&
        appendEntry(acl, ACL_EXTENDED_ALLOW, adminUuid,
                    {ACL_READ_DATA, ACL_WRITE_DATA, ACL_EXECUTE,
                     ACL_APPEND_DATA, ACL_DELETE, ACL_DELETE_CHILD,
                     ACL_READ_ATTRIBUTES, ACL_WRITE_ATTRIBUTES,
                     ACL_READ_EXTATTRIBUTES, ACL_WRITE_EXTATTRIBUTES,
                     ACL_READ_SECURITY, ACL_WRITE_SECURITY},
                    true);
    if (!built) {
        acl_free(acl);
        *error = QStringLiteral("Cannot build the handoff ACL entries");
        return false;
    }

    if (acl_set_file(nativePath.constData(), ACL_TYPE_EXTENDED, acl) != 0) {
        acl_free(acl);
        *error = QStringLiteral("Cannot apply the handoff ACL (errno %1)")
                     .arg(errno);
        return false;
    }
    acl_free(acl);
    return true;
}

}
