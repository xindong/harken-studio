#pragma once

#include <QString>
#include <QStringList>

namespace VNX {

enum class ProjectDataClass
{
    Raw,
    Derived,
    Preserved,
    TransactionControl,
    External,
    Unknown,
};

class ProjectDataPolicy
{
public:
    explicit ProjectDataPolicy(const QString& packBuildOutputDir = {},
                               const QString& buildWaveformOutputDir = {});
    static ProjectDataPolicy forProjectFile(const QString& projectFilePath);

    static QString normalizeRelativePath(const QString& path);
    static bool isSafeRelativePath(const QString& relativePath);
    ProjectDataClass classify(const QString& relativePath) const;
    bool includeInContentFingerprint(const QString& relativePath,
                                     bool isDirectory = false) const;
    bool protectByDefault(const QString& relativePath) const;

private:
    QStringList packOutputPaths_;
};

}
