#include "SecretStore.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace {

CFStringRef makeCFString(const QString& s)
{
    const QByteArray utf8 = s.toUtf8();
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8*>(utf8.constData()),
                                   utf8.size(),
                                   kCFStringEncodingUTF8,
                                   /*isExternalRepresentation=*/false);
}

}

namespace Harken::Platform::SecretStore {

bool erase(const QString& service, const QString& account)
{
    CFStringRef svc  = makeCFString(service);
    CFStringRef acct = makeCFString(account);
    if (!svc || !acct)
    {
        if (svc)  CFRelease(svc);
        if (acct) CFRelease(acct);
        return false;
    }

    const void* keys[]   = { kSecClass, kSecAttrService, kSecAttrAccount };
    const void* values[] = { kSecClassGenericPassword, svc, acct };
    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    CFRelease(svc);
    CFRelease(acct);
    return status == errSecSuccess || status == errSecItemNotFound;
}

bool store(const QString& service, const QString& account, const QString& secret)
{
    if (secret.isEmpty())
        return erase(service, account);

    erase(service, account);

    CFStringRef svc  = makeCFString(service);
    CFStringRef acct = makeCFString(account);
    const QByteArray secretUtf8 = secret.toUtf8();
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                  reinterpret_cast<const UInt8*>(secretUtf8.constData()),
                                  secretUtf8.size());
    if (!svc || !acct || !data)
    {
        if (svc)  CFRelease(svc);
        if (acct) CFRelease(acct);
        if (data) CFRelease(data);
        return false;
    }

    const void* keys[]   = { kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData };
    const void* values[] = { kSecClassGenericPassword, svc, acct, data };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 4,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const OSStatus status = SecItemAdd(attrs, nullptr);
    CFRelease(attrs);
    CFRelease(svc);
    CFRelease(acct);
    CFRelease(data);
    return status == errSecSuccess;
}

QString load(const QString& service, const QString& account)
{
    CFStringRef svc  = makeCFString(service);
    CFStringRef acct = makeCFString(account);
    if (!svc || !acct)
    {
        if (svc)  CFRelease(svc);
        if (acct) CFRelease(acct);
        return {};
    }

    const void* keys[] = {
        kSecClass, kSecAttrService, kSecAttrAccount,
        kSecReturnData, kSecMatchLimit,
    };
    const void* values[] = {
        kSecClassGenericPassword, svc, acct,
        kCFBooleanTrue, kSecMatchLimitOne,
    };
    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 5,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    CFRelease(svc);
    CFRelease(acct);

    if (status != errSecSuccess || !result)
        return {};

    CFDataRef data = static_cast<CFDataRef>(result);
    QString secret = QString::fromUtf8(
        reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
        CFDataGetLength(data));
    CFRelease(data);
    return secret;
}

}
