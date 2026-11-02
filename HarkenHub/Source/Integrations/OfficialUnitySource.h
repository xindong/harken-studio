#pragma once

#include "Packaging/PackageInventory.h"
#include "Security/ManifestVerifier.h"
#include "Update/ServerVersionCatalog.h"

#include <QTemporaryDir>
#include <memory>

namespace VNH {

struct OfficialUnitySourceRequest
{
    ServerVersionEntry release;
    QString buildId;
    QString packageVersion;
    QString localSource;
    QString cacheRoot;
    QUrl baseUrl;
};

struct OfficialUnitySourceResult
{
    bool ok = false;
    QString error;
    ReleaseManifest release;
    PackageInventory inventory;
    QString inventoryHash;
    QString sourcePath;
    bool reusedLocalSource = false;
    std::shared_ptr<QTemporaryDir> staging;
};

class OfficialUnitySource
{
public:
    static OfficialUnitySourceResult prepare(const OfficialUnitySourceRequest& request,
                                             const QByteArray& publicKey);
};

}
