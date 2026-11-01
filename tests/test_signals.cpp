// Unit tests for stage 4: signal formulas, the signal engine, and the offline
// labeler / evaluation. Built with ASan/UBSan by build.sh.

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "signals/signals.hpp"

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

}  // namespace

int main() {
    test_imbalance_mid_microprice();
    test_ofi_cases();
    test_signed_volume_and_decay();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all signal tests passed");
}
