#pragma once

// Sequence-number bookkeeping for one MoldUDP64 session, without recovery: a
// gap is counted and its messages are lost, so the books downstream can no
// longer be trusted (their unknown-ref counter usually shows it too).
// The first session name seen is locked in; other sessions are ignored.
// SeqStats counts by header: the FeedHandler counts payloads that do not match
// the header as malformed, which fails the run. The session locks on the first
// header seen, whatever it is.

#include <cstdint>
#include <string>
#include <string_view>

#include "net/mold.hpp"

namespace net {

enum class PacketKind { data, heartbeat, end_of_session, stale, wrong_session, malformed };

struct SeqStats {
    std::uint64_t packets = 0;        // data packets with at least one new message
    std::uint64_t messages = 0;       // new messages
    std::uint64_t heartbeats = 0;
    std::uint64_t gaps = 0;           // times the sequence jumped forward
    std::uint64_t lost_messages = 0;  // messages skipped over by those jumps
    std::uint64_t stale_packets = 0;  // packets with nothing new (duplicates, late arrivals)
    std::uint64_t wrong_session = 0;
};

struct SeqDecision {
    PacketKind kind;
    std::uint64_t skip;  // data: leading messages already seen, to be skipped
};

class SeqTracker {
public:
    SeqDecision on_header(const MoldHeader& h) {
        const std::string_view session = session_of(h);
        if (session_.empty()) {
            session_.assign(session);
        } else if (session != session_) {
            ++s_.wrong_session;
            return {PacketKind::wrong_session, 0};
        }
        const std::uint64_t seq = h.sequence.get();
        const std::uint16_t count = h.count.get();
        if (count == kEndOfSession) {
            note_jump(seq);
            ended_ = true;
            return {PacketKind::end_of_session, 0};
        }
        if (count == kHeartbeat) {
            note_jump(seq);
            ++s_.heartbeats;
            return {PacketKind::heartbeat, 0};
        }
        if (seq + count <= next_) {
            ++s_.stale_packets;
            return {PacketKind::stale, 0};
        }
        note_jump(seq);
        const std::uint64_t skip = next_ - seq;  // > 0 only on partial overlap
        next_ = seq + count;
        ++s_.packets;
        s_.messages += count - skip;
        return {PacketKind::data, skip};
    }

    [[nodiscard]] const SeqStats& stats() const noexcept { return s_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }
    [[nodiscard]] std::uint64_t next_expected() const noexcept { return next_; }

private:
    // Heartbeats and end-of-session carry the next sequence number, so they
    // reveal a gap at the tail just like a data packet does.
    void note_jump(std::uint64_t seq) {
        if (seq > next_) {
            ++s_.gaps;
            s_.lost_messages += seq - next_;
            next_ = seq;
        }
    }

    std::string session_;
    std::uint64_t next_ = 1;
    SeqStats s_;
    bool ended_ = false;
};

}  // namespace net
