#pragma once

#include <QString>
#include <QStringList>
#include <optional>

namespace VNX {

enum class AIPermissionMode { ReadOnly, Ask, Auto };
enum class AIToolProfile { ReadOnly, Authoring, Pack, Live };
enum class AIConfirmationPolicy { None, WhenPolicyRequires, Always };

AIPermissionMode parsePermissionMode(const QString& value);
QString toId(AIPermissionMode mode);
QString toId(AIToolProfile profile);

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
std::optional<AIToolProfile> parseToolProfile(const QString& value);
bool allowsWrites(AIPermissionMode mode);

struct AIPermissionMetadata
{
    const char* id;
    const char* label;
    const char* help;
};

const QList<AIPermissionMetadata>& permissionMetadata();

struct AIToolProfileMetadata
{
    AIToolProfile profile;
    const char* label;
};
const QList<AIToolProfileMetadata>& toolProfileMetadata();

constexpr unsigned int toolProfileBit(AIToolProfile profile)
{
    return 1u << static_cast<unsigned int>(profile);
}

class AIToolProfileSet
{
public:
    AIToolProfileSet() = default;
    AIToolProfileSet(unsigned int bits) : bits_(bits) {}
    AIToolProfileSet(const QStringList& ids);
    bool contains(AIToolProfile profile) const { return (bits_ & toolProfileBit(profile)) != 0; }
    bool contains(const QString& id) const;
    bool isEmpty() const { return bits_ == 0; }
    unsigned int bits() const { return bits_; }
    void add(AIToolProfile profile) { bits_ |= toolProfileBit(profile); }
private:
    unsigned int bits_ = 0;
};

bool requiresConfirmation(AIPermissionMode mode, AIConfirmationPolicy confirmation);

}
