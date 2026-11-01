#pragma once

// The consumer's per-packet logic, kept free of sockets so tests can drive it:
// check the MoldUDP64 header, track sequence numbers, skip messages already
// seen, and parse the new ones into any itch::parse handler (normally a
// BookBuilder). A packet whose message blocks do not match its header's
// count is counted as malformed. The parse is not bounded by the header's
// count, so extra blocks in a lying packet are applied before the mismatch is
// counted as malformed; the run fails either way.

#include <cstddef>
#include <cstdint>

#include "itch/parser.hpp"
#include "net/mold.hpp"
#include "net/seq_tracker.hpp"

namespace net {

template <class Handler>
class FeedHandler {
public:
    struct Result {
        PacketKind kind;
        std::uint64_t first_ts;  // data: ITCH timestamp of the first new message
        std::uint64_t messages;  // data: messages applied
    };

    explicit FeedHandler(Handler& h) : h_(h) {}

    Result on_packet(const unsigned char* pkt, std::size_t len) {
        if (len < sizeof(MoldHeader)) {
            ++malformed_;
            return {PacketKind::malformed, 0, 0};
        }
        const auto& hdr = *reinterpret_cast<const MoldHeader*>(pkt);
        const SeqDecision d = seq_.on_header(hdr);
        if (d.kind != PacketKind::data) return {d.kind, 0, 0};

        const unsigned char* const end = pkt + len;
        const unsigned char* p = skip_messages(pkt + sizeof(MoldHeader), end, d.skip);
        if (!p) {
            ++malformed_;
            return {PacketKind::malformed, 0, 0};
        }
        Result r{PacketKind::data, 0, 0};
        if (static_cast<std::size_t>(end - p) >= 2 + sizeof(itch::Header))
            r.first_ts = reinterpret_cast<const itch::Header*>(p + 2)->timestamp;
        const itch::ParseResult pr = itch::parse(p, end, h_);
        r.messages = pr.messages;
        if (pr.error != itch::ParseError::none || pr.messages != hdr.count.get() - d.skip) ++malformed_;
        return r;
    }

    [[nodiscard]] const SeqStats& seq_stats() const noexcept { return seq_.stats(); }
    [[nodiscard]] bool ended() const noexcept { return seq_.ended(); }
    [[nodiscard]] std::uint64_t malformed() const noexcept { return malformed_; }

private:
    Handler& h_;
    SeqTracker seq_;
    std::uint64_t malformed_ = 0;
};

}  // namespace net
