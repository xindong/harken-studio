#pragma once

#include <QObject>
#include <QString>

class QFileSystemWatcher;
class QJsonObject;
class QTimer;

namespace VNH {

class ProjectStore;

class ProjectIntakeConsumer : public QObject
{
    Q_OBJECT

public:
    static QString intakeDir();

    explicit ProjectIntakeConsumer(ProjectStore* store,
                                   QObject* parent = nullptr);

    void start();
    bool consumeAll();

signals:
    void projectsUpdated();

private:
    enum class RecordAction
    {
        Applied,
        Drop,
        Keep
    };

    RecordAction applyRecord(const QJsonObject& record);

    ProjectStore* store_ = nullptr;
    QFileSystemWatcher* watcher_ = nullptr;
    QTimer* debounceTimer_ = nullptr;
};

}
