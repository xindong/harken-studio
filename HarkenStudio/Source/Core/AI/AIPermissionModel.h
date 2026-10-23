#pragma once

#include <QString>
#include <QList>

namespace VNX {

enum class AIPermissionMode { ReadOnly, Ask, Auto };
enum class AIConfirmationPolicy { None, WhenPolicyRequires, Always };

AIPermissionMode parsePermissionMode(const QString& value);
QString toId(AIPermissionMode mode);

class AIPermissionSetting
{
public:
    AIPermissionSetting() = default;
    AIPermissionSetting(AIPermissionMode mode) : mode_(mode) {}
    AIPermissionSetting(const QString& value) : mode_(parsePermissionMode(value)) {}
    AIPermissionSetting& operator=(AIPermissionMode mode) { mode_ = mode; return *this; }
    AIPermissionSetting& operator=(const QString& value) { mode_ = parsePermissionMode(value); return *this; }
    operator AIPermissionMode() const { return mode_; }
    AIPermissionMode mode() const { return mode_; }
    friend bool operator==(AIPermissionSetting a, AIPermissionSetting b) { return a.mode_ == b.mode_; }
    friend bool operator!=(AIPermissionSetting a, AIPermissionSetting b) { return !(a == b); }
    friend bool operator==(AIPermissionSetting a, const QString& b) { return toId(a.mode_) == b; }
    friend bool operator==(const QString& a, AIPermissionSetting b) { return a == toId(b.mode_); }
    friend bool operator!=(AIPermissionSetting a, const QString& b) { return !(a == b); }
    friend bool operator!=(const QString& a, AIPermissionSetting b) { return !(a == b); }
private:
    AIPermissionMode mode_ = AIPermissionMode::Ask;
};

inline QString toId(AIPermissionSetting mode) { return toId(mode.mode()); }
bool allowsWrites(AIPermissionMode mode);

struct AIPermissionMetadata
{
    const char* id;
    const char* label;
    const char* help;
};

const QList<AIPermissionMetadata>& permissionMetadata();

bool requiresConfirmation(AIPermissionMode mode, AIConfirmationPolicy confirmation);

}
