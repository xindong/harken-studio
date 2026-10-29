#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QWidget;

namespace VNX {

class ProjectInstance : public QObject
{
    Q_OBJECT

public:
    enum class Status { Acquired, Forwarded, Failed };
    enum class ActivationPolicy { Allow, Deny };

    struct Acquisition {
        Status status = Status::Failed;
        std::shared_ptr<ProjectInstance> instance;
        QString error;
    };

    ~ProjectInstance() override;

    static Acquisition acquire(const QString& projectFile,
                               ActivationPolicy activation = ActivationPolicy::Allow);
    static QString identityForProject(const QString& projectFile);
    static void setActivationWindow(QWidget* window);
    static void activateWindow();

signals:
    void activationRequested();

private:
    struct NativeData;

    ProjectInstance();
    void receiveActivation();

    std::unique_ptr<NativeData> native_;
};

}
