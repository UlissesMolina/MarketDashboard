// Unit tests for the order map, both level policies, and the book builder.
// Built with ASan/UBSan by build.sh.

#include <cstdint>
#include <cstdio>
#include <random>
#include <unordered_map>

#include "book/book_builder.hpp"
#include "itch/parser.hpp"
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

using test::Wire;

// Random insert/find/erase against std::unordered_map, with a tiny table so
// probe chains wrap around the end and backward-shift deletion is exercised.
void test_order_map_matches_reference() {
    book::OrderMap m(4);  // 16 slots, grows as needed
    std::unordered_map<std::uint64_t, std::uint32_t> ref;
    std::mt19937_64 rng(12345);
    for (int step = 0; step < 200'000; ++step) {
        const std::uint64_t key = 1 + rng() % 2000;
        const int op = static_cast<int>(rng() % 3);
        if (op == 0) {
            book::Order* o = m.insert(key);
            CHECK((o != nullptr) == (ref.count(key) == 0));
            if (o) {
                o->shares = static_cast<std::uint32_t>(key * 7);
                ref[key] = o->shares;
            }
        } else if (op == 1) {
            book::Order* o = m.find(key);
            CHECK((o != nullptr) == (ref.count(key) == 1));
            if (o) CHECK(o->shares == ref[key]);
        } else {
            book::Order* o = m.find(key);
            if (o) {
                m.erase(o);
                ref.erase(key);
            }
        }
        if (m.size() != ref.size()) {
            CHECK(m.size() == ref.size());
            return;
        }
    }
    for (const auto& [k, v] : ref) {
        book::Order* o = m.find(k);
        CHECK(o && o->shares == v);
    }
}

constexpr std::uint16_t kLoc = 7;

// Message builders on top of test::Wire.
void add(Wire& w, std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price,
         std::uint16_t loc = kLoc) {
    w.header('A', loc, 0, 1).be(ref, 8).ch(side).be(shares, 4).str("TEST", 8).be(price, 4).end();
}
void exec(Wire& w, std::uint64_t ref, std::uint32_t shares, std::uint16_t loc = kLoc) {
    w.header('E', loc, 0, 1).be(ref, 8).be(shares, 4).be(1, 8).end();
}
void cancel(Wire& w, std::uint64_t ref, std::uint32_t shares) {
    w.header('X', kLoc, 0, 1).be(ref, 8).be(shares, 4).end();
}
void del(Wire& w, std::uint64_t ref) { w.header('D', kLoc, 0, 1).be(ref, 8).end(); }
void replace(Wire& w, std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares, std::uint32_t price) {
    w.header('U', kLoc, 0, 1).be(old_ref, 8).be(new_ref, 8).be(shares, 4).be(price, 4).end();
}
void system_event(Wire& w, char code) { w.header('S', 0, 0, 1).ch(code).end(); }
void trading_action(Wire& w, char state) {
    w.header('H', kLoc, 0, 1).str("TEST", 8).ch(state).ch(' ').str("    ", 4).end();
}

template <template <bool> class L>
book::BookBuilder<L> run(const Wire& w) {
    book::BookBuilder<L> b(4);
    const auto r = itch::parse(w.data(), w.data_end(), b);
    CHECK(r.error == itch::ParseError::none);
    return b;
}

template <template <bool> class L>
void test_levels_aggregate_and_bbo() {
    Wire w;
    add(w, 1, 'B', 100, 10'000);
    add(w, 2, 'B', 200, 10'000);  // same level
    add(w, 3, 'B', 50, 10'100);   // new best bid
    add(w, 4, 'B', 70, 9'900);    // behind
    add(w, 5, 'S', 300, 10'300);
    add(w, 6, 'S', 10, 10'200);   // new best ask
    auto b = run<L>(w);
    const auto q = b.book(kLoc).bbo();
    CHECK(q.bid == 10'100 && q.bid_shares == 50);
    CHECK(q.ask == 10'200 && q.ask_shares == 10);
    CHECK(b.book(kLoc).bids.depth() == 3);
    CHECK(b.book(kLoc).asks.depth() == 2);
    CHECK(b.live_orders() == 6);
    CHECK(b.violations().hard() == 0);
    CHECK(b.audit() == 0);
}

template <template <bool> class L>
void test_execute_cancel_delete() {
    Wire w;
    add(w, 1, 'B', 100, 10'000);
    add(w, 2, 'B', 200, 10'000);
    exec(w, 1, 40);    // partial
    cancel(w, 2, 50);  // partial
    exec(w, 1, 60);    // completes order 1: it leaves the book
    auto b = run<L>(w);
    CHECK(b.live_orders() == 1);
    const auto& best = b.book(kLoc).bids.best();
    CHECK(best.price == 10'000 && best.shares == 150 && best.orders == 1);
    CHECK(b.violations().hard() == 0);
    CHECK(b.audit() == 0);

    Wire w2 = w;
    del(w2, 2);
    auto b2 = run<L>(w2);
    CHECK(b2.live_orders() == 0);
    CHECK(b2.book(kLoc).bids.empty());
    CHECK(b2.audit() == 0);
}

template <template <bool> class L>
void test_replace_keeps_side_and_moves_level() {
    Wire w;
    add(w, 1, 'S', 100, 10'500);
    add(w, 2, 'S', 100, 10'600);
    replace(w, 1, 9, 300, 10'400);
    auto b = run<L>(w);
    CHECK(b.live_orders() == 2);
    const auto q = b.book(kLoc).bbo();
    CHECK(q.ask == 10'400 && q.ask_shares == 300);
    CHECK(q.bid == 0);
    CHECK(b.book(kLoc).asks.depth() == 2);  // 10'500 level emptied and removed
    CHECK(b.violations().hard() == 0);
    CHECK(b.audit() == 0);
}

template <template <bool> class L>
void test_violations_are_counted() {
    Wire w;
    add(w, 1, 'B', 100, 10'000);
    exec(w, 99, 10);       // unknown ref
    exec(w, 1, 150);       // overfill
    add(w, 2, 'B', 5, 1);
    add(w, 2, 'B', 5, 1);  // duplicate ref
    exec(w, 2, 1, kLoc + 1);  // wrong locate
    auto b = run<L>(w);
    const auto& v = b.violations();
    CHECK(v.unknown_ref == 1);
    CHECK(v.overfill == 1);
    CHECK(v.duplicate_add == 1);
    CHECK(v.locate_mismatch == 1);
    CHECK(b.audit() == 0);  // the book stays self-consistent despite bad input
}

template <template <bool> class L>
void test_crossed_book_only_flagged_when_open_and_trading() {
    // Pre-open: a crossed book is allowed.
    Wire w;
    trading_action(w, 'T');
    add(w, 1, 'B', 100, 10'100);
    add(w, 2, 'S', 100, 10'000);
    auto pre = run<L>(w);
    CHECK(pre.violations().crossed_open == 0);
    CHECK(pre.violations().crossed_other == 1);

    // Market open and trading: crossed and locked are violations.
    Wire o;
    system_event(o, 'Q');
    trading_action(o, 'T');
    add(o, 1, 'B', 100, 10'100);
    add(o, 2, 'S', 100, 10'000);  // crossed
    add(o, 3, 'S', 100, 10'100);  // still crossed (best ask unchanged)
    del(o, 2);                    // now locked at 10'100, but deletes don't check
    add(o, 4, 'B', 1, 9'000);     // add on the far side: book is locked
    auto open = run<L>(o);
    CHECK(open.violations().crossed_open == 2);
    CHECK(open.violations().locked_open == 1);

    // Halted while open: not a violation.
    Wire h;
    system_event(h, 'Q');
    trading_action(h, 'H');
    add(h, 1, 'B', 100, 10'100);
    add(h, 2, 'S', 100, 10'000);
    auto halted = run<L>(h);
    CHECK(halted.violations().crossed_open == 0);
    CHECK(halted.violations().crossed_other == 1);
}

template <template <bool> class L>
void test_audit_detects_corruption() {
    Wire w;
    add(w, 1, 'B', 100, 10'000);
    add(w, 2, 'S', 100, 10'100);
    auto b = run<L>(w);
    CHECK(b.audit() == 0);
    b.book_for_test(kLoc).bids.add(10'000, 5);  // level now disagrees with its orders
    CHECK(b.audit() == 2);                        // expected level and actual level differ
    b.book_for_test(kLoc).asks.add(9'000, 1);    // phantom level with no orders
    CHECK(b.audit() == 3);
}

template <template <bool> class L>
void test_scheduled_audits() {
    Wire w;
    w.header('A', kLoc, 0, 100).be(1, 8).ch('B').be(10, 4).str("TEST", 8).be(50, 4).end();
    w.header('A', kLoc, 0, 200).be(2, 8).ch('B').be(10, 4).str("TEST", 8).be(50, 4).end();
    w.header('A', kLoc, 0, 300).be(3, 8).ch('B').be(10, 4).str("TEST", 8).be(50, 4).end();
    book::BookBuilder<L> b(4, {150, 300});
    itch::parse(w.data(), w.data_end(), b);
    CHECK(b.audits().size() == 2);
    if (b.audits().size() == 2) {
        CHECK(b.audits()[0].timestamp == 200 && b.audits()[0].live_orders == 1);  // runs before the add
        CHECK(b.audits()[1].timestamp == 300 && b.audits()[1].live_orders == 2);
        CHECK(b.audits()[0].mismatches == 0 && b.audits()[1].mismatches == 0);
    }
}

template <template <bool> class L>
void run_book_tests() {
    test_audit_detects_corruption<L>();
    test_scheduled_audits<L>();
    test_levels_aggregate_and_bbo<L>();
    test_execute_cancel_delete<L>();
    test_replace_keeps_side_and_moves_level<L>();
    test_violations_are_counted<L>();
    test_crossed_book_only_flagged_when_open_and_trading<L>();
}

}  // namespace

int main() {
    test_order_map_matches_reference();
    run_book_tests<book::VectorLevels>();
    run_book_tests<book::MapLevels>();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all book tests passed");
}
