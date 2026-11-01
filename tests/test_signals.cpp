// Unit tests for stage 4: signal formulas, the signal engine, and the offline
// labeler / evaluation. Built with ASan/UBSan by build.sh.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "signals/signals.hpp"
#include "itch/parser.hpp"
#include "signals/signal_engine.hpp"
#include "signals/study.hpp"
#include "wire.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

book::Bbo quote(std::uint32_t bid, std::uint64_t bid_sz, std::uint32_t ask, std::uint64_t ask_sz) {
    book::Bbo q;
    q.bid = bid;
    q.bid_shares = bid_sz;
    q.ask = ask;
    q.ask_shares = ask_sz;
    return q;
}

void test_imbalance_mid_microprice() {
    const book::Bbo q = quote(10'000, 300, 10'002, 100);
    CHECK(signals::two_sided(q));
    CHECK(near(signals::mid(q), 10'001));
    CHECK(near(signals::imbalance(q), 0.5));
    CHECK(near(signals::microprice(q), (10'000.0 * 100 + 10'002.0 * 300) / 400));
    // Identity: microprice - mid = spread / 2 * imbalance.
    CHECK(near(signals::microprice(q) - signals::mid(q), 2.0 / 2 * signals::imbalance(q)));

    const book::Bbo empty_ask = quote(10'000, 300, 0, 0);
    CHECK(!signals::two_sided(empty_ask));
    CHECK(!signals::two_sided(quote(10'002, 1, 10'000, 1)));  // crossed
    CHECK(near(signals::imbalance(book::Bbo{}), 0.0));
}

void test_ofi_cases() {
    const book::Bbo prev = quote(10'000, 300, 10'002, 200);
    // Bid size up at the same price: +100. Ask unchanged: +200 - 200 = 0.
    CHECK(near(signals::ofi(prev, quote(10'000, 400, 10'002, 200)), 100));
    // Bid price up: +new bid size (50). Ask unchanged: 0.
    CHECK(near(signals::ofi(prev, quote(10'001, 50, 10'002, 200)), 50));
    // Bid price down: -old bid size (300).
    CHECK(near(signals::ofi(prev, quote(9'999, 80, 10'002, 200)), -300));
    // Ask size down at the same price: +200 - 150 = +50 (buy pressure).
    CHECK(near(signals::ofi(prev, quote(10'000, 300, 10'002, 150)), 50));
    // Ask price down: -new ask size (70).
    CHECK(near(signals::ofi(prev, quote(10'000, 300, 10'001, 70)), -70));
    // Ask price up: +old ask size (200).
    CHECK(near(signals::ofi(prev, quote(10'000, 300, 10'003, 10)), 200));
    // No change: 0.
    CHECK(near(signals::ofi(prev, prev), 0));
}

void test_signed_volume_and_decay() {
    CHECK(near(signals::signed_volume('B', 100), -100));  // hit a resting buy: sell-initiated
    CHECK(near(signals::signed_volume('S', 100), 100));

    signals::DecayingSum s;
    const double tau = 1e9;
    s.add(1'000'000'000, 100, tau);
    CHECK(near(s.at(1'000'000'000, tau), 100));
    CHECK(near(s.at(2'000'000'000, tau), 100 * std::exp(-1.0), 1e-9));
    s.add(2'000'000'000, -50, tau);
    CHECK(near(s.at(2'000'000'000, tau), 100 * std::exp(-1.0) - 50, 1e-9));
    CHECK(near(s.at(1'500'000'000, tau), s.at(2'000'000'000, tau)));  // never decays backwards
}

void test_engine_bbo_changes_and_ofi() {
    signals::SignalEngine e(1e9);
    const std::uint16_t loc = 3;
    CHECK(e.update_bbo(loc, 100, quote(10'000, 300, 0, 0)));     // one-sided: no OFI yet
    CHECK(e.update_bbo(loc, 200, quote(10'000, 300, 10'002, 200)));  // prev one-sided: no OFI
    CHECK(!e.update_bbo(loc, 300, quote(10'000, 300, 10'002, 200)));  // unchanged touch
    CHECK(e.update_bbo(loc, 400, quote(10'000, 400, 10'002, 200)));   // OFI +100
    CHECK(e.bbo_changes(loc) == 3);
    CHECK(e.bbo(loc) == quote(10'000, 400, 10'002, 200));
    const signals::Snapshot s = e.snapshot(loc, 400);
    CHECK(near(s.ofi, 100));
    CHECK(near(s.imbalance, (400.0 - 200) / 600));
    const double m = 10'001;
    CHECK(near(s.micro_bps, (signals::microprice(quote(10'000, 400, 10'002, 200)) - m) / m * 1e4));
    CHECK(near(e.snapshot(loc, 400 + 1'000'000'000).ofi, 100 * std::exp(-1.0), 1e-9));
    CHECK(e.last_ts() == 400);
}

void test_engine_trade_flow() {
    signals::SignalEngine e(1e9);
    e.on_execution(5, 1'000, 'B', 100, 10'000);  // sell-initiated
    e.on_execution(5, 1'000, 'S', 30, 10'002);   // buy-initiated
    CHECK(near(e.snapshot(5, 1'000).flow, -70));
    CHECK(e.executions(5) == 2);
    CHECK(near(e.snapshot(6, 1'000).flow, 0));  // other symbols untouched
}

void test_engine_as_book_listener() {
    test::Wire w;
    w.header('A', 9, 0, 100).be(1, 8).ch('B').be(100, 4).str("TEST", 8).be(10'000, 4).end();
    w.header('A', 9, 0, 200).be(2, 8).ch('S').be(100, 4).str("TEST", 8).be(10'002, 4).end();
    w.header('A', 9, 0, 300).be(3, 8).ch('B').be(50, 4).str("TEST", 8).be(9'990, 4).end();   // behind: no touch change
    w.header('E', 9, 0, 400).be(2, 8).be(40, 4).be(1, 8).end();                               // buy-initiated 40
    book::BookBuilder<book::VectorLevels, signals::SignalEngine> b(4);
    itch::parse(w.data(), w.data_end(), b);
    const signals::SignalEngine& e = b.listener();
    CHECK(e.bbo(9) == b.book(9).bbo());
    CHECK(e.bbo_changes(9) == 3);  // add bid, add ask, ask size 100 -> 60
    CHECK(e.executions(9) == 1);
    CHECK(near(e.snapshot(9, 400).flow, 40));
    CHECK(near(e.snapshot(9, 400).ofi, 40));  // ask size down at the same price: +100 - 60
}

void test_labeler_resolves_both_labels() {
    signals::Labeler lab(150);  // horizon 150 ns
    const std::array<double, signals::kNumSignals> x{1, 2, 3, 4};
    lab.on_change(100, 10'000);
    lab.add(100, x);  // s0
    lab.on_change(200, 10'000);  // size change only: mid unchanged
    lab.add(200, x);  // s1
    lab.on_change(300, 10'002);  // mid up: s0, s1 next_move +1; s0 horizon (250) passed at mid 10'000
    lab.add(300, x);  // s2
    lab.on_change(1'000, 9'998);  // mid down: s2 next_move -1; s1 (350) and s2 (450) resolve
    lab.add(1'000, x);  // s3: never resolved

    const auto& s = lab.samples();
    CHECK(s.size() == 4);
    if (s.size() == 4) {
        CHECK(s[0].next_move == 1 && near(s[0].fwd_bps, 0.0));
        CHECK(s[1].next_move == 1 && near(s[1].fwd_bps, 2.0));  // (10002 - 10000) / 10000 * 1e4
        CHECK(s[2].next_move == -1 && near(s[2].fwd_bps, 0.0));
        CHECK(!s[3].labeled());
        CHECK(s[0].labeled() && s[2].mid == 10'002 && s[2].x[3] == 4);
    }
}

void test_engine_ofi_ignores_one_sided_and_crossed() {
    signals::SignalEngine e(1e9);
    const std::uint16_t loc = 1;
    e.update_bbo(loc, 100, quote(10'000, 300, 10'002, 200));
    e.update_bbo(loc, 100, quote(10'000, 400, 10'002, 200));  // OFI +100
    CHECK(near(e.snapshot(loc, 100).ofi, 100));
    CHECK(e.update_bbo(loc, 100, quote(10'001, 50, 0, 0)));  // ask side empties
    CHECK(near(e.snapshot(loc, 100).ofi, 100));
    CHECK(near(e.snapshot(loc, 100).imbalance, 0.0));
    CHECK(e.update_bbo(loc, 100, quote(10'000, 400, 10'002, 200)));  // back; prev was one-sided
    CHECK(near(e.snapshot(loc, 100).ofi, 100));
    CHECK(e.update_bbo(loc, 100, quote(10'003, 70, 10'002, 30)));  // crossed
    CHECK(near(e.snapshot(loc, 100).ofi, 100));
    CHECK(near(e.snapshot(loc, 100).imbalance, 0.0) && near(e.snapshot(loc, 100).micro_bps, 0.0));
    CHECK(e.update_bbo(loc, 100, quote(10'000, 100, 10'002, 50)));  // uncrossed; prev was crossed
    CHECK(near(e.snapshot(loc, 100).ofi, 100));
}

void test_labeler_same_timestamp_keeps_last_sample() {
    signals::Labeler lab(150);
    const std::array<double, signals::kNumSignals> x{1, 1, 1, 1}, y{2, 2, 2, 2};
    lab.add(50, x);  // no mid yet: ignored
    CHECK(lab.samples().empty());
    lab.on_change(100, 10'000);
    lab.add(100, x);
    lab.on_change(100, 10'002);  // same event, mid moves up
    lab.add(100, y);
    lab.on_change(500, 10'001);  // mid down
    const auto& s = lab.samples();
    CHECK(s.size() == 1);
    if (s.size() == 1) {
        CHECK(s[0].ts == 100 && s[0].x[0] == 2 && s[0].mid == 10'002);
        CHECK(s[0].next_move == -1);  // m3 vs m2, not m2 vs m
        CHECK(near(s[0].fwd_bps, 0.0));
    }
}

void test_labeler_drop_at_removes_same_event_samples() {
    signals::Labeler lab(150);
    const std::array<double, signals::kNumSignals> x{1, 1, 1, 1};
    lab.on_change(100, 10'000);
    lab.add(100, x);
    lab.drop_at(100);  // the event ended one-sided
    lab.on_change(500, 10'002);
    CHECK(lab.samples().empty());
}

void test_labeler_horizon_boundary() {
    signals::Labeler lab(150);
    const std::array<double, signals::kNumSignals> x{1, 2, 3, 4};
    lab.on_change(100, 10'000);
    lab.add(100, x);
    lab.on_change(250, 10'004);  // exactly t + H: the post-change mid is the one in force
    lab.on_change(260, 10'006);
    const auto& s = lab.samples();
    CHECK(s.size() == 1);
    if (s.size() == 1) CHECK(near(s[0].fwd_bps, (10'004.0 - 10'000) / 10'000 * 1e4));
}

std::vector<signals::Sample> synthetic(double scale, std::uint64_t n) {
    std::vector<signals::Sample> v;
    for (std::uint64_t i = 0; i < n; ++i) {
        const double x = (double(i) - double(n - 1) / 2) * scale;  // centered, no zeros for even n
        signals::Sample s{i, 100.0, {x, 0, 0, 0}, x > 0 ? 1 : -1, x / scale};
        v.push_back(s);
    }
    return v;
}

void test_evaluate_perfect_signal_across_scales() {
    const auto a = synthetic(1.0, 100);
    const auto b = synthetic(1000.0, 100);  // same signal in different units
    std::vector<signals::Sample> unlabeled(5, signals::Sample{0, 100.0, {1, 0, 0, 0}, 0,
                                                              std::numeric_limits<double>::quiet_NaN()});
    const signals::Report r = signals::evaluate({&a, &b, &unlabeled}, 0);
    CHECK(r.n == 200);
    CHECK(near(r.hit_rate, 1.0));
    CHECK(near(r.corr, 1.0, 1e-9));
    for (std::size_t d = 0; d < 10; ++d) CHECK(r.decile_n[d] == 20);
    for (std::size_t d = 1; d < 10; ++d) CHECK(r.decile_bps[d] > r.decile_bps[d - 1]);

    const signals::Report flat = signals::evaluate({&a}, 1);  // signal 1 is always 0
    CHECK(flat.hits_n == 0 && near(flat.corr, 0.0));
}

}  // namespace

int main() {
    test_imbalance_mid_microprice();
    test_ofi_cases();
    test_signed_volume_and_decay();
    test_engine_bbo_changes_and_ofi();
    test_engine_trade_flow();
    test_engine_ofi_ignores_one_sided_and_crossed();
    test_engine_as_book_listener();
    test_labeler_resolves_both_labels();
    test_labeler_same_timestamp_keeps_last_sample();
    test_labeler_drop_at_removes_same_event_samples();
    test_labeler_horizon_boundary();
    test_evaluate_perfect_signal_across_scales();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all signal tests passed");
}
