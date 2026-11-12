#ifndef HARKEN_API_MONITOR_IDENTITY_CUT_H
#define HARKEN_API_MONITOR_IDENTITY_CUT_H

#include "MonitorIdentityCodec.h"
#include "MonitorHostSymbolCodec.h"

namespace harken::monitor {

// Both sender and receiver hash canonical identity rows, including their source
// envelope. Session routing and local delivery stamps are deliberately excluded.
inline std::uint64_t IdentityRowChecksum(std::uint64_t checksum,
    VnMonitorRecordHeader header, const VnMonitorIdentityFactV1& value) noexcept {
    header.monitorSessionEpoch = 0;
    header.deliverySequence = 0;
    std::uint8_t bytes[kIdentityRecordBytes]{};
    PackHeader(bytes, header);
    ExecutionPayloadWriter writer{bytes + kPackedEnvelopeBytes};
    VisitIdentityPayload(writer, value);
    return IdentityChecksum(checksum, bytes, sizeof(bytes));
}

// One bounded verifier per in-flight cut. Missing, duplicated, reordered or
// cross-session rows cannot be repaired merely by receiving an End record.
class MonitorIdentityCutVerifier {
public:
    bool Begin(const VnMonitorRecordHeader& header, const VnMonitorIdentityCutBeginV1& value) noexcept {
        *this = {};
        if (!ValidateIdentityBegin(header, value)) return false;
        begin_ = value;
        engine_ = header.engineMonitorInstanceId;
        epoch_ = header.monitorSessionEpoch;
        next_ = header.sourceSequence;
        for (auto count : value.namespaceTrueCount) expected_ += count;
        if (next_ == UINT64_MAX || expected_ > UINT64_MAX - next_ - 1) return false;
        ++next_;
        begun_ = true;
        return true;
    }

    bool Row(const VnMonitorRecordHeader& header, const VnMonitorIdentityFactV1& value) noexcept {
        if (!begun_ || ended_ || !ValidateIdentityFact(header, value) ||
            !Matches(header, value.stateCutId, value.identityRevision) ||
            header.sourceSequence != next_ || received_ == expected_ ||
            (value.fact.factFlags & kMonitorFactIdentityIncomplete) != 0 || symbolField_ != 0) {
            failed_ = true;
            return false;
        }
        const auto ns = value.fact.subjectNamespace;
        if (counts_[ns] == begin_.namespaceTrueCount[ns]) {
            failed_ = true;
            return false;
        }
        ++counts_[ns];
        ++received_;
        ++next_;
        checksum_ = IdentityRowChecksum(checksum_, header, value);
        return true;
    }

    bool Row(const VnMonitorRecordHeader& header, const VnMonitorHostObjectSymbolV1& value) noexcept {
        constexpr auto ns = VN_MONITOR_SUBJECT_HOST_SYMBOL;
        const bool sameHost = symbolField_ == 0 || (symbolEmitter_ == header.instanceHandle &&
            symbolSession_ == value.hostSession && symbolSequence_ == value.hostSequence &&
            symbolRevision_ == value.registrationRevision && symbolFlags_ == value.flags);
        if (!begun_ || ended_ || !ValidateHostSymbol(header, value) ||
            !Matches(header, value.stateCutId, value.cutRevision) || header.sourceSequence != next_ ||
            received_ == expected_ || counts_[ns] == begin_.namespaceTrueCount[ns] ||
            value.field != symbolField_ + 1 || !sameHost) {
            failed_ = true;
            return false;
        }
        symbolEmitter_ = header.instanceHandle;
        symbolSession_ = value.hostSession;
        symbolSequence_ = value.hostSequence;
        symbolRevision_ = value.registrationRevision;
        symbolFlags_ = value.flags;
        symbolField_ = value.field == VN_MONITOR_HOST_SYMBOL_SCENE ? 0 : value.field;
        ++counts_[ns];
        ++received_;
        ++next_;
        checksum_ = HostSymbolRowChecksum(checksum_, header, value);
        return true;
    }

    bool End(const VnMonitorRecordHeader& header, const VnMonitorIdentityCutEndV1& value) noexcept {
        const bool valid = begun_ && !ended_ && !failed_ && ValidateIdentityEnd(header, value) &&
            Matches(header, value.stateCutId, value.identityRevision) && header.sourceSequence == next_ &&
            received_ == expected_ && symbolField_ == 0 &&
            value.result == VN_MONITOR_STATE_CUT_COMPLETE && value.checksum == checksum_;
        bool countsMatch = true;
        for (unsigned i = 0; i != 9; ++i)
            countsMatch &= counts_[i] == begin_.namespaceTrueCount[i] && counts_[i] == value.namespaceEmittedCount[i];
        ended_ = true;
        failed_ |= !valid || !countsMatch;
        return Complete();
    }

    bool Complete() const noexcept { return begun_ && ended_ && !failed_; }
    std::uint64_t Received() const noexcept { return received_; }
    std::uint64_t Checksum() const noexcept { return checksum_; }

private:
    bool Matches(const VnMonitorRecordHeader& header, std::uint64_t cut, std::uint64_t revision) const noexcept {
        return header.engineMonitorInstanceId == engine_ && header.monitorSessionEpoch == epoch_ &&
            cut == begin_.stateCutId && revision == begin_.identityRevision;
    }
    VnMonitorIdentityCutBeginV1 begin_{};
    std::uint64_t engine_ = 0, epoch_ = 0, next_ = 0, expected_ = 0, received_ = 0;
    std::uint64_t counts_[9]{};
    std::uint64_t symbolEmitter_ = 0, symbolSession_ = 0, symbolSequence_ = 0, symbolRevision_ = 0;
    std::uint32_t symbolField_ = 0, symbolFlags_ = 0;
    std::uint64_t checksum_ = kIdentityChecksumSeed;
    bool begun_ = false, ended_ = false, failed_ = false;
};

} // namespace harken::monitor

#endif
