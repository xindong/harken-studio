#include "Platform/UpdateBridgeTask.h"

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>

namespace VNH {

namespace {

NSString* const kDaemonPlistName =
    @"com.harkensound.harkenhub.updatebridge.plist";

}

QString updateBridgeTaskName()
{
    return QStringLiteral("com.harkensound.harkenhub.updatebridge");
}

bool updateBridgeTaskAvailable()
{
    if (@available(macOS 13.0, *)) {
        @autoreleasepool {
            SMAppService* service =
                [SMAppService daemonServiceWithPlistName:kDaemonPlistName];
            return service.status == SMAppServiceStatusEnabled;
        }
    }
    return false;
}

bool registerUpdateBridgeTask(const QString& hubExePath, QString* error)
{
    Q_UNUSED(hubExePath);
    if (@available(macOS 13.0, *)) {
        @autoreleasepool {
            SMAppService* service =
                [SMAppService daemonServiceWithPlistName:kDaemonPlistName];
            if (service.status == SMAppServiceStatusEnabled)
                return true;
            NSError* nsError = nil;
            const BOOL registered = [service registerAndReturnError:&nsError];
            if (registered ||
                service.status == SMAppServiceStatusEnabled ||
                service.status == SMAppServiceStatusRequiresApproval)
                return true;
            if (error) {
                *error = nsError
                             ? QString::fromNSString(
                                   nsError.localizedDescription)
                             : QStringLiteral(
                                   "The update bridge daemon could not be "
                                   "registered");
            }
            return false;
        }
    }
    if (error)
        *error = QStringLiteral(
            "The update bridge requires macOS 13 or later");
    return false;
}

bool unregisterUpdateBridgeTask(QString* error)
{
    if (@available(macOS 13.0, *)) {
        @autoreleasepool {
            SMAppService* service =
                [SMAppService daemonServiceWithPlistName:kDaemonPlistName];
            if (service.status == SMAppServiceStatusNotRegistered ||
                service.status == SMAppServiceStatusNotFound)
                return true;
            NSError* nsError = nil;
            if ([service unregisterAndReturnError:&nsError])
                return true;
            if (error) {
                *error = nsError
                             ? QString::fromNSString(
                                   nsError.localizedDescription)
                             : QStringLiteral(
                                   "The update bridge daemon could not be "
                                   "removed");
            }
            return false;
        }
    }
    return true;
}

bool runUpdateBridgeTask(QString* error)
{
    Q_UNUSED(error);
    return true;
}

}
