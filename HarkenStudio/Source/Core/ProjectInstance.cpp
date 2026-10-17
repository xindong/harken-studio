#include "ProjectInstance.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#ifdef Q_OS_WIN
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <windows.h>
#include <sddl.h>
#elif defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#endif

namespace VNX {

namespace {

constexpr int kActivationTimeoutMs = 5000;
constexpr int kRetryIntervalMs = 50;

QHash<QString, std::weak_ptr<ProjectInstance>>& instances()
{
    static QHash<QString, std::weak_ptr<ProjectInstance>> value;
    return value;
}

QPointer<QWidget> activationWindow;
bool pendingActivation = false;

void focusWindow()
{
    QWidget* window = activationWindow;
    if (window && window->isMinimized())
        window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    if (QWidget* modal = QApplication::activeModalWidget())
        window = modal;
    if (!window)
        return;
    if (window->isMinimized())
        window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    window->show();
    window->raise();
    window->activateWindow();
    if (QWindow* handle = window->windowHandle())
        handle->requestActivate();
#ifdef Q_OS_WIN
    SetForegroundWindow(reinterpret_cast<HWND>(window->winId()));
#endif
}

QString currentUserIdentity()
{
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray storage(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (!GetTokenInformation(token, TokenUser, storage.data(), size, &size)) {
        CloseHandle(token);
        return {};
    }
    LPWSTR sid = nullptr;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.constData());
    const bool converted = ConvertSidToStringSidW(user->User.Sid, &sid);
    QString result = converted ? QString::fromWCharArray(sid) : QString();
    if (sid)
        LocalFree(sid);
    CloseHandle(token);
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId))
        return {};
    if (!result.isEmpty())
        result += QStringLiteral(".%1").arg(sessionId);
    return result;
#elif defined(Q_OS_MACOS)
    return QString::number(getuid());
#else
    return {};
#endif
}

QString channelName(const QString& identity, const QString& user)
{
    const QByteArray digest = QCryptographicHash::hash(
        user.toUtf8() + '\0' + identity.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("com.harkensound.studio.project.%1")
        .arg(QString::fromLatin1(digest));
}

#ifdef Q_OS_WIN

constexpr qint64 kMaxMessageBytes = 1024;

QByteArray readMessage(QLocalSocket& socket, QDeadlineTimer& deadline)
{
    QByteArray buffer;
    while (!deadline.hasExpired()) {
        buffer.append(socket.readAll());
        if (buffer.size() > kMaxMessageBytes)
            return {};
        if (buffer.endsWith('\n'))
            return buffer;
        if (socket.state() != QLocalSocket::ConnectedState)
            return {};
        socket.waitForReadyRead(static_cast<int>(deadline.remainingTime()));
    }
    return {};
}

bool forwardActivation(const QString& name, QDeadlineTimer& deadline)
{
    QLocalSocket socket;
    socket.setReadBufferSize(kMaxMessageBytes + 1);
    socket.connectToServer(name);
    if (!socket.waitForConnected(
            qMin(kRetryIntervalMs, static_cast<int>(deadline.remainingTime()))))
        return false;
    const QJsonObject hello = QJsonDocument::fromJson(readMessage(socket, deadline)).object();
    const qint64 processId = hello.value(QStringLiteral("pid")).toInteger();
    if (hello.value(QStringLiteral("protocol")).toInt() != 1 ||
        processId <= 0 || processId > MAXDWORD)
        return false;
    AllowSetForegroundWindow(static_cast<DWORD>(processId));
    socket.write("activate\n");
    socket.flush();
    return readMessage(socket, deadline) == QByteArrayLiteral("activated\n");
}

#endif

}

struct ProjectInstance::NativeData
{
#ifdef Q_OS_WIN
    HANDLE mutex = nullptr;
    bool ownsMutex = false;
    std::unique_ptr<QLocalServer> server;
    int activeClients = 0;

    ~NativeData()
    {
        server.reset();
        if (ownsMutex)
            ReleaseMutex(mutex);
        if (mutex)
            CloseHandle(mutex);
    }

    bool listen(const QString& name, ProjectInstance* owner)
    {
        server = std::make_unique<QLocalServer>();
        server->setSocketOptions(QLocalServer::UserAccessOption);
        server->setMaxPendingConnections(8);
        QObject::connect(server.get(), &QLocalServer::newConnection, owner, [this, owner]() {
            while (QLocalSocket* client = server->nextPendingConnection()) {
                if (activeClients >= 8) {
                    client->abort();
                    client->deleteLater();
                    continue;
                }
                ++activeClients;
                client->setReadBufferSize(kMaxMessageBytes + 1);
                QObject::connect(client, &QObject::destroyed, owner,
                                 [this]() { --activeClients; });
                QTimer::singleShot(kActivationTimeoutMs, client, [client]() {
                    client->abort();
                    client->deleteLater();
                });
                QObject::connect(client, &QLocalSocket::disconnected,
                                 client, &QObject::deleteLater);
                const auto buffer = std::make_shared<QByteArray>();
                QObject::connect(client, &QLocalSocket::readyRead, owner,
                                 [owner, client, buffer]() {
                    buffer->append(client->readAll());
                    if (buffer->size() > kMaxMessageBytes ||
                        (buffer->contains('\n') && *buffer != "activate\n")) {
                        client->abort();
                        return;
                    }
                    if (*buffer != "activate\n")
                        return;
                    owner->receiveActivation();
                    client->write("activated\n");
                    client->disconnectFromServer();
                });
                const QJsonObject hello{
                    {QStringLiteral("protocol"), 1},
                    {QStringLiteral("pid"), QCoreApplication::applicationPid()}};
                client->write(QJsonDocument(hello).toJson(QJsonDocument::Compact) + '\n');
                client->flush();
            }
        });
        return server->listen(name);
    }
#elif defined(Q_OS_MACOS)
    CFMessagePortRef port = nullptr;
    CFRunLoopSourceRef source = nullptr;

    ~NativeData()
    {
        if (source) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
            CFRelease(source);
        }
        if (port) {
            CFMessagePortInvalidate(port);
            CFRelease(port);
        }
    }

    static CFDataRef receive(CFMessagePortRef, SInt32 messageId, CFDataRef data, void* context)
    {
        if (messageId != 1 || (data && CFDataGetLength(data) != 0))
            return nullptr;
        static_cast<ProjectInstance*>(context)->receiveActivation();
        const UInt8 response = 1;
        return CFDataCreate(kCFAllocatorDefault, &response, 1);
    }
#endif
};

ProjectInstance::ProjectInstance()
    : native_(std::make_unique<NativeData>())
{
}

ProjectInstance::~ProjectInstance() = default;

QString ProjectInstance::identityForProject(const QString& projectFile)
{
    if (projectFile.trimmed().isEmpty())
        return {};
    QFileInfo file(projectFile);
    if (const QString canonicalFile = file.canonicalFilePath(); !canonicalFile.isEmpty())
        file.setFile(canonicalFile);
    QString path = file.isDir() ? file.absoluteFilePath() : file.absolutePath();
    QStringList suffix;
    QString canonical;
    for (;;) {
        const QFileInfo ancestor(path);
        canonical = ancestor.canonicalFilePath();
        if (!canonical.isEmpty())
            break;
        const QString parent = ancestor.absolutePath();
        if (parent == path)
            return {};
        suffix.prepend(ancestor.fileName());
        path = parent;
    }
    if (!suffix.isEmpty())
        canonical = QDir(canonical).filePath(suffix.join('/'));
    canonical = QDir::cleanPath(canonical);
#ifdef Q_OS_WIN
    canonical = canonical.toCaseFolded();
#endif
    return canonical;
}

ProjectInstance::Acquisition ProjectInstance::acquire(const QString& projectFile)
{
    if (!QCoreApplication::instance() ||
        QThread::currentThread() != QCoreApplication::instance()->thread())
        return {Status::Failed, {}, QStringLiteral("Project activation requires the application thread.")};
    const QString identity = identityForProject(projectFile);
    const QString user = currentUserIdentity();
    if (identity.isEmpty() || user.isEmpty())
        return {Status::Failed, {}, QStringLiteral("Could not identify the project activation channel.")};
    auto& registry = instances();
    if (const auto existing = registry.value(identity).lock())
        return {Status::Acquired, existing, {}};
    for (auto it = registry.begin(); it != registry.end();) {
        if (it.value().expired())
            it = registry.erase(it);
        else
            ++it;
    }
    const QString name = channelName(identity, user);
    auto instance = std::shared_ptr<ProjectInstance>(new ProjectInstance());
    QDeadlineTimer deadline(kActivationTimeoutMs);
#ifdef Q_OS_WIN
    const QString mutexName = QStringLiteral("Local\\") + name;
    instance->native_->mutex = CreateMutexW(
        nullptr, FALSE, reinterpret_cast<LPCWSTR>(mutexName.utf16()));
    if (!instance->native_->mutex)
        return {Status::Failed, {}, QStringLiteral("Could not create the project activation guard.")};
    do {
        const DWORD state = WaitForSingleObject(instance->native_->mutex, 0);
        if (state == WAIT_OBJECT_0 || state == WAIT_ABANDONED) {
            instance->native_->ownsMutex = true;
            if (!instance->native_->listen(name, instance.get()))
                return {Status::Failed, {}, QStringLiteral("Could not start the project activation channel.")};
            registry.insert(identity, instance);
            return {Status::Acquired, instance, {}};
        }
        if (state == WAIT_FAILED)
            return {Status::Failed, {}, QStringLiteral("Could not inspect the project activation guard.")};
        if (forwardActivation(name, deadline))
            return {Status::Forwarded, {}, {}};
        if (!deadline.hasExpired())
            QThread::msleep(kRetryIntervalMs);
    } while (!deadline.hasExpired());
#elif defined(Q_OS_MACOS)
    const QByteArray encodedName = name.toUtf8();
    CFStringRef portName = CFStringCreateWithBytes(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(encodedName.constData()),
        encodedName.size(), kCFStringEncodingUTF8, false);
    do {
        CFMessagePortContext context{0, instance.get(), nullptr, nullptr, nullptr};
        instance->native_->port = CFMessagePortCreateLocal(
            kCFAllocatorDefault, portName, NativeData::receive, &context, nullptr);
        if (instance->native_->port) {
            instance->native_->source = CFMessagePortCreateRunLoopSource(
                kCFAllocatorDefault, instance->native_->port, 0);
            if (!instance->native_->source) {
                CFRelease(portName);
                return {Status::Failed, {}, QStringLiteral("Could not start the project activation channel.")};
            }
            CFRunLoopAddSource(CFRunLoopGetMain(), instance->native_->source,
                               kCFRunLoopCommonModes);
            CFRelease(portName);
            registry.insert(identity, instance);
            return {Status::Acquired, instance, {}};
        }
        if (CFMessagePortRef remote = CFMessagePortCreateRemote(kCFAllocatorDefault, portName)) {
            CFDataRef reply = nullptr;
            const CFTimeInterval timeout = deadline.remainingTime() / 1000.0;
            const CFTimeInterval sendTimeout = qMin(0.1, timeout);
            const SInt32 result = CFMessagePortSendRequest(
                remote, 1, nullptr, sendTimeout, timeout - sendTimeout,
                kCFRunLoopDefaultMode, &reply);
            const bool activated = result == kCFMessagePortSuccess && reply &&
                CFDataGetLength(reply) == 1 && CFDataGetBytePtr(reply)[0] == 1;
            if (reply)
                CFRelease(reply);
            CFRelease(remote);
            if (activated) {
                CFRelease(portName);
                return {Status::Forwarded, {}, {}};
            }
        }
        if (!deadline.hasExpired())
            QThread::msleep(kRetryIntervalMs);
    } while (!deadline.hasExpired());
    CFRelease(portName);
#endif
    return {Status::Failed, {}, QStringLiteral("The project is already opening in another Studio instance, but that instance did not respond to activation. Try again after it finishes opening.")};
}

void ProjectInstance::setActivationWindow(QWidget* window)
{
    activationWindow = window;
    if (window && pendingActivation) {
        pendingActivation = false;
        focusWindow();
    }
}

void ProjectInstance::activateWindow()
{
    pendingActivation = true;
    focusWindow();
}

void ProjectInstance::receiveActivation()
{
    activateWindow();
    emit activationRequested();
}

}
