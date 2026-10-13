
#pragma once

#include "AssetTypes.h"
#include <QObject>
#include <QString>
#include <QStringList>

namespace VNX {

struct ProjectFileBinding;

struct ImportResult
{
    bool success = false;
    bool deduplicated = false;
    uint32_t sourceID = 0;
    QString relativePath;
    QString absolutePath;
    AssetError error = AssetError::None;
    QString errorMessage;
    AudioMetadata metadata;
};

class AssetImporter : public QObject
{
    Q_OBJECT

public:
    explicit AssetImporter(QObject* parent = nullptr);
    ~AssetImporter() override;

    void setProjectRoot(const QString& root);


    ImportResult importFile(const QString& externalPath,
                            ProjectFileBinding* targetBinding,
                            const QString& sourceFolder = {});


    AudioMetadata analyzeFile(const QString& path);

    static LoopInfo readWavLoopInfo(const QString& path);

    static ValidationFlags validateAudio(const QString& path);


    bool validateFile(const QString& path, QString& errorMsg);

    static QStringList supportedFormats();

    static bool isFormatSupported(const QString& path);

    static QString computeContentHash(const QString& path);

signals:
    void importProgress(int current, int total, const QString& fileName);

    void importComplete(int successCount, int failCount);

    void importError(const QString& path, const QString& error);

private:

    QString copyToAudioSources(const QString& externalPath, const QString& sourceFolder);

    QString detectFormat(const QString& path);

    AudioMetadata readWavMetadata(const QString& path);

    AudioMetadata readMp3Metadata(const QString& path);

    AudioMetadata readFlacMetadata(const QString& path);

    AudioMetadata readOggMetadata(const QString& path);

    AudioMetadata readAiffMetadata(const QString& path);

    QString projectRoot_;
};

}
