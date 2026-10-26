#include "itchbook/replay.hpp"

namespace itchbook {

// The replay tools are templates, so they live in replay.hpp. Only this
// non-template helper is compiled here.

void TopOfBookColumns::add(std::uint64_t t, const BookTop& top) {
  time.push_back(t);
  bid_price.push_back(top.bids.empty() ? 0 : top.bids[0].price);
  bid_shares.push_back(top.bids.empty() ? 0 : top.bids[0].shares);
  ask_price.push_back(top.asks.empty() ? 0 : top.asks[0].price);
  ask_shares.push_back(top.asks.empty() ? 0 : top.asks[0].shares);
}

}  // namespace itchbook
