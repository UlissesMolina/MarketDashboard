// Unit tests for the zero-copy parser, using hand-built wire bytes.
// Built with ASan/UBSan by build.sh so out-of-bounds reads fail loudly.

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

#include "itch/parser.hpp"
#include "wire.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using test::Wire;

struct Recorder {
    std::vector<itch::AddOrder> adds;
    std::vector<itch::OrderReplace> replaces;
    std::vector<itch::CrossTrade> crosses;
    std::vector<char> unknown;
    int deletes = 0;

    void on(const itch::AddOrder& m) { adds.push_back(m); }
    void on(const itch::OrderReplace& m) { replaces.push_back(m); }
    void on(const itch::CrossTrade& m) { crosses.push_back(m); }
    void on(const itch::OrderDelete&) { ++deletes; }
    void on_unknown(char t, const unsigned char*, std::uint16_t) { unknown.push_back(t); }
};

void test_add_order_fields() {
    Wire w;
    // 34200 s after midnight = 09:30:00, plus 123 ns; exercises all 48 timestamp bits.
    const std::uint64_t ts = 34'200'000'000'123ULL;
    w.header('A', 0x1234, 7, ts)
        .be(0x0102030405060708ULL, 8)
        .ch('B')
        .be(100, 4)
        .str("AAPL", 8)
        .be(1'665'500, 4)  // $166.5500
        .end();

    Recorder r;
    const auto res = itch::parse(w.data(), w.data_end(), r);
    CHECK(res.error == itch::ParseError::none);
    CHECK(res.messages == 1);
    CHECK(res.offset == w.bytes.size());
    CHECK(r.adds.size() == 1);
    const auto& a = r.adds.at(0);
    CHECK(a.hdr.type == 'A');
    CHECK(a.hdr.stock_locate == 0x1234);
    CHECK(a.hdr.tracking_number == 7);
    CHECK(a.hdr.timestamp == ts);
    CHECK(a.order_ref == 0x0102030405060708ULL);
    CHECK(a.side == 'B');
    CHECK(a.shares == 100);
    CHECK(itch::trim(a.stock) == "AAPL");
    CHECK(a.price == 1'665'500);
}

void test_timestamp_high_bits() {
    Wire w;
    const std::uint64_t ts = 0xFFEE'DDCC'BBAAULL;  // max-ish 48-bit value
    w.header('D', 1, 0, ts).be(42, 8).end();
    struct H {
        std::uint64_t ts = 0;
        void on(const itch::OrderDelete& m) { ts = m.hdr.timestamp; }
    } h;
    itch::parse(w.data(), w.data_end(), h);
    CHECK(h.ts == ts);
}

void test_multiple_and_unhandled_types() {
    Wire w;
    w.header('S', 0, 0, 1).ch('O').end();                       // not handled by Recorder
    w.header('D', 5, 0, 2).be(99, 8).end();
    w.header('U', 5, 0, 3).be(99, 8).be(100, 8).be(300, 4).be(5000, 4).end();
    w.header('Q', 5, 0, 4).be(1'000'000, 8).str("QQQ", 8).be(1'600'000, 4).be(77, 8).ch('C').end();

    Recorder r;
    const auto res = itch::parse(w.data(), w.data_end(), r);
    CHECK(res.error == itch::ParseError::none);
    CHECK(res.messages == 4);
    CHECK(r.deletes == 1);
    CHECK(r.replaces.size() == 1);
    CHECK(r.replaces.at(0).original_order_ref == 99);
    CHECK(r.replaces.at(0).new_order_ref == 100);
    CHECK(r.replaces.at(0).shares == 300);
    CHECK(r.replaces.at(0).price == 5000);
    CHECK(r.crosses.size() == 1);
    CHECK(r.crosses.at(0).shares == 1'000'000);
    CHECK(itch::trim(r.crosses.at(0).stock) == "QQQ");
    CHECK(r.crosses.at(0).cross_type == 'C');
    CHECK(r.unknown.empty());
}

void test_unknown_type_is_skipped() {
    Wire w;
    w.header('z', 0, 0, 1).be(0, 4).end();
    w.header('D', 0, 0, 2).be(1, 8).end();
    Recorder r;
    const auto res = itch::parse(w.data(), w.data_end(), r);
    CHECK(res.error == itch::ParseError::none);
    CHECK(res.messages == 2);
    CHECK(r.unknown.size() == 1 && r.unknown.at(0) == 'z');
    CHECK(r.deletes == 1);
}

void test_truncated_message() {
    Wire w;
    w.header('D', 0, 0, 1).be(1, 8).end();
    w.header('D', 0, 0, 2).be(2, 8).end();
    const std::size_t first_len = 2 + sizeof(itch::OrderDelete);
    // Copy all but the last 3 bytes so the second message is cut short.
    std::vector<unsigned char> cut(w.bytes.begin(), w.bytes.end() - 3);
    Recorder r;
    const auto res = itch::parse(cut.data(), cut.data() + cut.size(), r);
    CHECK(res.error == itch::ParseError::truncated);
    CHECK(res.messages == 1);
    CHECK(res.offset == first_len);
    CHECK(r.deletes == 1);
}

void test_dangling_length_byte() {
    Wire w;
    w.header('D', 0, 0, 1).be(1, 8).end();
    w.bytes.push_back(0);  // half a length prefix
    Recorder r;
    const auto res = itch::parse(w.data(), w.data_end(), r);
    CHECK(res.error == itch::ParseError::truncated);
    CHECK(res.messages == 1);
}

void test_bad_length_for_known_type() {
    Wire w;
    w.header('D', 0, 0, 1).be(1, 8).ch('x').end();  // one byte too long
    Recorder r;
    const auto res = itch::parse(w.data(), w.data_end(), r);
    CHECK(res.error == itch::ParseError::bad_length);
    CHECK(res.messages == 0);
    CHECK(res.offset == 0);
    CHECK(r.deletes == 0);
}

void test_zero_length() {
    const unsigned char bytes[] = {0, 0, 0, 0};
    Recorder r;
    const auto res = itch::parse(bytes, bytes + sizeof bytes, r);
    CHECK(res.error == itch::ParseError::bad_length);
}

void test_empty_buffer() {
    Recorder r;
    const unsigned char* none = nullptr;
    const auto res = itch::parse(none, none, r);
    CHECK(res.error == itch::ParseError::none);
    CHECK(res.messages == 0);
}

}  // namespace

int main() {
    test_add_order_fields();
    test_timestamp_high_bits();
    test_multiple_and_unhandled_types();
    test_unknown_type_is_skipped();
    test_truncated_message();
    test_dangling_length_byte();
    test_bad_length_for_known_type();
    test_zero_length();
    test_empty_buffer();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all parser tests passed");
}
