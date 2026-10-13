#include <harken/Harken.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() != 4) {
        return 2;
    }
    VnConfig config{};
    config.structSize = sizeof(config);
    config.sampleRate = 48000;
    config.deviceMode = VN_DEVICE_REAL;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.blockFrames = 1024;
    config.maxVoices = 16;
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 16;
    VnResult result = Vn_Init(&config);
    if (result != VN_OK) {
        QTextStream(stderr) << "Vn_Init: " << result << Qt::endl;
        return 3;
    }
    const auto directory = QDir(arguments[1]);
    const auto path = directory.filePath(QStringLiteral("Init.pack")).toUtf8();
    const auto root = directory.absolutePath().toUtf8();
    VnPackLoadDesc load{};
    load.structSize = sizeof(load);
    load.flags = VN_PACK_LOAD_FLAG_ROOT;
    load.utf8Path = path.constData();
    load.utf8ContainmentRoot = root.constData();
    VnInitPackInspectReport inspection{};
    inspection.structSize = sizeof(inspection);
    result = Vn_InspectInitPack(&load, &inspection);
    if (result != VN_OK) {
        QTextStream(stderr) << "Vn_InspectInitPack [" << path << "]: " << result
                            << ", report=" << inspection.result
                            << ", bytes=" << QFileInfo(QString::fromUtf8(path)).size()
                            << Qt::endl;
        Vn_Term();
        return 4;
    }
    const auto contentPath = directory.filePath(QStringLiteral("Content.pack")).toUtf8();
    VnCanonicalPackSetMemberV1 members[2]{};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    members[0].structSize = sizeof(members[0]);
    members[0].flags = VN_CANONICAL_PACK_SET_MEMBER_ROOT;
    members[0].packStableId = inspection.packStableId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = path.constData();
    members[0].utf8ContainmentRoot = root.constData();
    members[1].structSize = sizeof(members[1]);
    members[1].flags = VN_CANONICAL_PACK_SET_MEMBER_ROOT;
    members[1].packStableId = 22;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.constData();
    members[1].utf8ContainmentRoot = root.constData();
    rows[0].structSize = sizeof(rows[0]);
    rows[1].structSize = sizeof(rows[1]);
    VnCanonicalPackSetActivationDescV1 activation{};
    activation.structSize = sizeof(activation);
    activation.setRevision = 1;
    activation.memberCount = 2;
    activation.memberStructSize = sizeof(members[0]);
    activation.members = members;
    activation.outputMemberCapacity = 2;
    activation.outputMemberStructSize = sizeof(rows[0]);
    activation.outputMembers = rows;
    VnCanonicalPackSetActivationReportV1 report{};
    report.structSize = sizeof(report);
    result = Vn_ActivateCanonicalPackSetV1(&activation, &report);
    if (result != VN_OK) {
        QTextStream(stderr) << "Vn_ActivateCanonicalPackSetV1: " << result
                            << ", packId=" << inspection.packStableId
                            << ", catalogEntries=" << inspection.catalogEntries
                            << Qt::endl;
        Vn_Term();
        return 4;
    }
    VnRemoteStatus status{};
    status.structSize = sizeof(status);
    if (Vn_RemoteGetStatus(&status) != VN_OK || status.state != VN_REMOTE_STOPPED) {
        Vn_Term();
        return 5;
    }
    VnRemoteConfig remote{};
    remote.structSize = sizeof(remote);
    remote.flags = VN_REMOTE_DISCOVERY_ENABLED;
    remote.discoveryPort = arguments[3].toUInt();
    remote.applicationName = "Remote acceptance game";
    remote.projectName = "Remote foundation";
    result = Vn_RemoteStart(&remote);
    if (result != VN_OK || Vn_RemoteGetStatus(&status) != VN_OK) {
        QTextStream(stderr) << "Vn_RemoteStart: " << result << Qt::endl;
        Vn_Term();
        return 6;
    }
    QJsonObject ready;
    ready.insert(QStringLiteral("port"), static_cast<int>(status.tcpPort));
    ready.insert(QStringLiteral("pid"), static_cast<int>(status.processId));
    ready.insert(QStringLiteral("instance"), QString::number(status.instanceId, 16));
    ready.insert(QStringLiteral("version"), QString::fromUtf8(status.engineVersion));
    QTextStream(stdout) << QJsonDocument(ready).toJson(QJsonDocument::Compact) << Qt::endl;
    QTimer updates;
    bool eventProbeDone = false;
    QObject::connect(&updates, &QTimer::timeout, &application, [&] {
        const auto update = Vn_Update();
        if (update != VN_OK) {
            QTextStream(stderr) << "Vn_Update: " << update << Qt::endl;
            application.exit(7);
        }
        if (!eventProbeDone) {
            QFile probe(directory.filePath(QStringLiteral("event-probe.json")));
            if (probe.open(QIODevice::ReadOnly)) {
                const auto request = QJsonDocument::fromJson(probe.readAll()).object();
                VnObjectId variable{};
                if (!harken::ParseObjectId(request.value(QStringLiteral("variable")).toString().toStdString(), &variable)) {
                    application.exit(9);
                    return;
                }
                Vn_SetValue(variable, 0.35f);
                VnPostActionDesc post{};
                post.structSize = sizeof(post);
                if (!harken::ParseObjectId(request.value(QStringLiteral("action")).toString().toStdString(), &post.actionId)) {
                    application.exit(9);
                    return;
                }
                VnActionFiringHandle firing = 0;
                const auto posted = Vn_PostAction(&post, &firing);
                if (posted == VN_BACKPRESSURE) {
                    return;
                }
                if (posted != VN_OK) {
                    QTextStream(stderr) << "Vn_PostAction [event probe]: " << posted << Qt::endl;
                    application.exit(10);
                    return;
                }
                eventProbeDone = true;
                post.actionId = {UINT64_C(0x3da91a64b9474560), UINT64_C(999000111)};
                Vn_PostAction(&post, &firing);
            }
        }
        if (QFileInfo::exists(arguments[2])) {
            application.quit();
        }
    });
    updates.start(5);
    QTimer::singleShot(30000, &application, [&] { application.exit(8); });
    const int exitCode = application.exec();
    const auto terminated = Vn_Term();
    return terminated == VN_OK ? exitCode : 9;
}
