#pragma once

#include <QString>

namespace VNH {

inline QString handoffAclSddl()
{
    return QStringLiteral(
        "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OIIO;FA;;;CO)(A;;0x1200ab;;;BU)");
}

inline QString updaterAclSddl()
{
    return QStringLiteral(
        "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)");
}

bool applyHandoffAcl(const QString& directoryPath, QString* error);

bool applyDirectoryAcl(const QString& directoryPath, const QString& sddl,
                       QString* error);

}
