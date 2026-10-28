#pragma once

// Nasdaq TotalView-ITCH 5.0 message layouts.
// Every struct overlays the wire bytes exactly; the static_asserts at the bottom
// pin each size to the spec so a layout mistake fails the build, not the parse.
// Prices are Price(4): unsigned integer with 4 implied decimal places, unless noted.

#include <string_view>

#include "itch/endian.hpp"

namespace itch {

struct Header {
    char type;
    be16 stock_locate;
    be16 tracking_number;
    be48 timestamp;  // nanoseconds since midnight
};

// Alpha(8) stock symbol, left-justified and space-padded.
using Symbol = char[8];

[[nodiscard]] inline std::string_view trim(const Symbol& s) noexcept {
    std::size_t n = sizeof(Symbol);
    while (n > 0 && s[n - 1] == ' ') --n;
    return {s, n};
}

// --- System and reference data ---------------------------------------------

struct SystemEvent {  // 'S'
    Header hdr;
    char event_code;  // O S Q M E C
};

struct StockDirectory {  // 'R'
    Header hdr;
    Symbol stock;
    char market_category;
    char financial_status;
    be32 round_lot_size;
    char round_lots_only;
    char issue_classification;
    char issue_sub_type[2];
    char authenticity;
    char short_sale_threshold;
    char ipo_flag;
    char luld_ref_price_tier;
    char etp_flag;
    be32 etp_leverage_factor;
    char inverse_indicator;
};

struct StockTradingAction {  // 'H'
    Header hdr;
    Symbol stock;
    char trading_state;  // H P Q T
    char reserved;
    char reason[4];
};

struct RegShoRestriction {  // 'Y'
    Header hdr;
    Symbol stock;
    char reg_sho_action;
};

struct MarketParticipantPosition {  // 'L'
    Header hdr;
    char mpid[4];
    Symbol stock;
    char primary_market_maker;
    char market_maker_mode;
    char market_participant_state;
};

struct MwcbDeclineLevel {  // 'V'  Price(8): 8 implied decimals
    Header hdr;
    be64 level1;
    be64 level2;
    be64 level3;
};

struct MwcbStatus {  // 'W'
    Header hdr;
    char breached_level;
};

struct IpoQuotingPeriodUpdate {  // 'K'
    Header hdr;
    Symbol stock;
    be32 ipo_quotation_release_time;  // seconds since midnight
    char ipo_quotation_release_qualifier;
    be32 ipo_price;
};

struct LuldAuctionCollar {  // 'J'
    Header hdr;
    Symbol stock;
    be32 reference_price;
    be32 upper_collar_price;
    be32 lower_collar_price;
    be32 extension;
};

struct OperationalHalt {  // 'h'
    Header hdr;
    Symbol stock;
    char market_code;
    char halt_action;
};

// --- Order book messages ----------------------------------------------------

struct AddOrder {  // 'A'
    Header hdr;
    be64 order_ref;
    char side;  // 'B' or 'S'
    be32 shares;
    Symbol stock;
    be32 price;
};

struct AddOrderMpid {  // 'F'
    Header hdr;
    be64 order_ref;
    char side;
    be32 shares;
    Symbol stock;
    be32 price;
    char attribution[4];
};

struct OrderExecuted {  // 'E'
    Header hdr;
    be64 order_ref;
    be32 executed_shares;
    be64 match_number;
};

struct OrderExecutedWithPrice {  // 'C'
    Header hdr;
    be64 order_ref;
    be32 executed_shares;
    be64 match_number;
    char printable;
    be32 execution_price;
};

struct OrderCancel {  // 'X'  partial cancel
    Header hdr;
    be64 order_ref;
    be32 cancelled_shares;
};

struct OrderDelete {  // 'D'
    Header hdr;
    be64 order_ref;
};

struct OrderReplace {  // 'U'  cancel-replace; new order keeps the original side
    Header hdr;
    be64 original_order_ref;
    be64 new_order_ref;
    be32 shares;
    be32 price;
};

// --- Trade messages ---------------------------------------------------------

struct Trade {  // 'P'  execution against a non-displayed order
    Header hdr;
    be64 order_ref;
    char side;
    be32 shares;
    Symbol stock;
    be32 price;
    be64 match_number;
};

struct CrossTrade {  // 'Q'  opening / closing / halt / IPO cross
    Header hdr;
    be64 shares;
    Symbol stock;
    be32 cross_price;
    be64 match_number;
    char cross_type;  // O C H I
};

struct BrokenTrade {  // 'B'
    Header hdr;
    be64 match_number;
};

// --- Auction and retail interest -------------------------------------------

struct Noii {  // 'I'  net order imbalance indicator
    Header hdr;
    be64 paired_shares;
    be64 imbalance_shares;
    char imbalance_direction;
    Symbol stock;
    be32 far_price;
    be32 near_price;
    be32 current_reference_price;
    char cross_type;
    char price_variation_indicator;
};

struct RetailPriceImprovement {  // 'N'
    Header hdr;
    Symbol stock;
    char interest_flag;
};

struct DirectListingPriceDiscovery {  // 'O'  added in 2020
    Header hdr;
    Symbol stock;
    char open_eligibility_status;
    be32 minimum_allowable_price;
    be32 maximum_allowable_price;
    be32 near_execution_price;
    be64 near_execution_time;
    be32 lower_price_range_collar;
    be32 upper_price_range_collar;
};

// Sizes from the ITCH 5.0 spec (message length, excluding the 2-byte length prefix).
static_assert(sizeof(Header) == 11);
static_assert(sizeof(SystemEvent) == 12);
static_assert(sizeof(StockDirectory) == 39);
static_assert(sizeof(StockTradingAction) == 25);
static_assert(sizeof(RegShoRestriction) == 20);
static_assert(sizeof(MarketParticipantPosition) == 26);
static_assert(sizeof(MwcbDeclineLevel) == 35);
static_assert(sizeof(MwcbStatus) == 12);
static_assert(sizeof(IpoQuotingPeriodUpdate) == 28);
static_assert(sizeof(LuldAuctionCollar) == 35);
static_assert(sizeof(OperationalHalt) == 21);
static_assert(sizeof(AddOrder) == 36);
static_assert(sizeof(AddOrderMpid) == 40);
static_assert(sizeof(OrderExecuted) == 31);
static_assert(sizeof(OrderExecutedWithPrice) == 36);
static_assert(sizeof(OrderCancel) == 23);
static_assert(sizeof(OrderDelete) == 19);
static_assert(sizeof(OrderReplace) == 35);
static_assert(sizeof(Trade) == 44);
static_assert(sizeof(CrossTrade) == 40);
static_assert(sizeof(BrokenTrade) == 19);
static_assert(sizeof(Noii) == 50);
static_assert(sizeof(RetailPriceImprovement) == 20);
static_assert(sizeof(DirectListingPriceDiscovery) == 48);

}  // namespace itch
