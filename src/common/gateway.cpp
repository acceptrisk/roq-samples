/* Copyright (c) 2017-2018, Hans Erik Thrane */

#include "common/gateway.h"

#include <roq/logging.h>
#include <roq/stream.h>

#include <gflags/gflags.h>

#include <algorithm>

#include "common/instrument.h"

namespace examples {
namespace common {

namespace {
const char *TRADER = "Trader";  // FIXME(thraneh): introduce an enum for this!
}  // namespace

// Currently only supporting a single gateway...
DEFINE_string(gtc_open, "gtc_open", "Order template.");
DEFINE_string(gtc_close, "gtc_close", "Order template.");

Gateway::Gateway(
    roq::Strategy::Dispatcher& dispatcher,
    const std::string& name)
    : _dispatcher(dispatcher),
      _name(name),
      _gtc_open(FLAGS_gtc_open),
      _gtc_close(FLAGS_gtc_close) {
}

void Gateway::on(const roq::TimerEvent& event) {
  // TODO(thraneh): check order requests timing out
}

void Gateway::on(const roq::DownloadBeginEvent& event) {
  // set download flag (block strategy from creating new orders)
  _download = true;
  LOG(INFO) << "download=" << (_download ? "true" : "false");
  // reset all variables tracking order management state
  _order_manager_ready = false;
  _order_traded_quantity.clear();
  // note! order_max_id must *NEVER* be reset
  _live_orders.clear();
}

void Gateway::on(const roq::DownloadEndEvent& event) {
  // max order id
  update_max_order_id(event.download_end.max_order_id);
  // reset download flag
  _download = false;
  LOG(INFO) << "download=" << (_download ? "true" : "false");
}

void Gateway::on(const roq::GatewayStatusEvent& event) {
  const auto& gateway_status = event.gateway_status;
  // return early if it's not the gateway's order management status
  if (std::strcmp(TRADER, gateway_status.name) != 0)
    return;
  // ready?
  auto order_manager_ready = gateway_status.status == roq::GatewayState::Ready;
  if (_order_manager_ready != order_manager_ready) {
    _order_manager_ready = order_manager_ready;
    LOG(INFO) << "order_manager_ready=" << (_order_manager_ready ? "true" : "false");
  }
}

// The mapping between order_id and instrument is normally set up
// when the order is created. However, this pattern can only be used
// until between downloads. The download phase requires us to reset
// all state before replaying the day's order updates. Thus, we must
// require the caller to map exchange/instrument to the instrument
// object before calling this method. But only during download...
void Gateway::on(const roq::OrderUpdateEvent& event, Instrument *instrument) {
  const auto& order_update = event.order_update;
  auto order_id = order_update.order_id;
  // max order id
  update_max_order_id(order_id);
  // pass update event to instrument
  if (instrument) {
    // download
    LOG_IF(FATAL, is_order_live(order_id)) << "Unexpected";
    if (roq::is_order_live(order_update.order_status))
      _live_orders[order_id] = instrument;
    instrument->on(event);
  } else {
    // live
    auto iter = _live_orders.find(order_id);
    if (iter != _live_orders.end())
      (*iter).second->on(event);
  }
}

void Gateway::update_max_order_id(uint32_t order_id) {
  auto max_order_id = std::max(order_id, _max_order_id);
  if (max_order_id != _max_order_id) {
    _max_order_id = max_order_id;
    LOG(INFO) << "max_order_id=" << _max_order_id;
  }
}

uint32_t Gateway::create_order(
    const std::string& exchange,
    const std::string& symbol,
    roq::TradeDirection direction,
    double quantity,
    double price,
    const std::string& order_template,
    Instrument& handler) {
  if (is_ready() == false)
    throw roq::NotReady();
  auto order_id = ++_max_order_id;
  LOG_IF(FATAL, is_order_live(order_id)) << "Unexpected";
  roq::CreateOrder create_order {
    .order_id       = order_id,
    .order_template = order_template.c_str(),
    .exchange       = exchange.c_str(),
    .symbol         = symbol.c_str(),
    .direction      = direction,
    .quantity       = quantity,
    .limit_price    = price,
  };
  LOG(INFO) << "create_order=" << create_order;
  _dispatcher.send(create_order, _name.c_str());
  _live_orders[order_id] = &handler;
  return order_id;
}

void Gateway::modify_order(
    uint32_t order_id,
    double quantity_change,
    double limit_price,
    Instrument& instrument) {
  if (is_order_live(order_id) == false)
    throw roq::OrderNotLive();
  if (is_ready() == false)
    throw roq::NotReady();
  roq::ModifyOrder modify_order {
    .order_id        = order_id,
    .quantity_change = quantity_change,
    .limit_price     = limit_price,
  };
  LOG(INFO) << "modify_order=" << modify_order;
  _dispatcher.send(modify_order, _name.c_str());
}

void Gateway::cancel_order(
    uint32_t order_id,
    Instrument& instrument) {
  if (is_order_live(order_id) == false)
    throw roq::OrderNotLive();
  if (is_ready() == false)
    throw roq::NotReady();
  roq::CancelOrder cancel_order {
    .order_id = order_id,
  };
  LOG(INFO) << "cancel_order=" << cancel_order;
  _dispatcher.send(cancel_order, _name.c_str());
}

void Gateway::on(const roq::CreateOrderAckEvent& event) {
  const auto& create_order_ack = event.create_order_ack;
  auto iter = _live_orders.find(create_order_ack.order_id);
  if (iter != _live_orders.end())
    (*iter).second->on(event);
  if (create_order_ack.failure)
    _live_orders.erase(create_order_ack.order_id);
}

void Gateway::on(const roq::ModifyOrderAckEvent& event) {
  const auto& modify_order_ack = event.modify_order_ack;
  auto iter = _live_orders.find(modify_order_ack.order_id);
  if (iter != _live_orders.end())
    (*iter).second->on(event);
  // there's nothing to do if a failure is being signalled...
}

void Gateway::on(const roq::CancelOrderAckEvent& event) {
  const auto& cancel_order_ack = event.cancel_order_ack;
  auto iter = _live_orders.find(cancel_order_ack.order_id);
  if (iter != _live_orders.end())
    (*iter).second->on(event);
  // there's nothing to do if a failure is being signalled...
}

bool Gateway::is_order_live(uint32_t order_id) const {
  return _live_orders.find(order_id) != _live_orders.end();
}

// The interface is generic an supposed to work for a multitude of
// gateways. The client therefore has no access to market specific
// fields. Those extra fields are only visible to the gateway. The
// client can, however, leverage such extra fields through templates.
// The gateway will ensure all order updates includes the original
// name of the template. The client can therefore safely compare
// the name of the template with that it used to create the order.
// Returns true if the order template is an "open".
// Returns false if the order template is a "close".
// Terminate program execution if the order template is unknown.
bool Gateway::parse_open_close(const char *order_template) const {
  if (_gtc_open.compare(order_template) == 0)
    return true;
  if (_gtc_close.compare(order_template) == 0)
    return false;
  LOG(FATAL) << "Unknown order_template=\"" << order_template << "\"";
}

double Gateway::get_fill_quantity(const roq::OrderUpdate& order_update) {
  auto& previous = _order_traded_quantity[order_update.order_id];
  auto result = std::max(0.0, order_update.traded_quantity - previous);
  previous = order_update.traded_quantity;
  return result;
}

}  // namespace common
}  // namespace examples