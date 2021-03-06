#pragma once

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <caf/actor.hpp>
#include <caf/actor_addr.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/broadcast_downstream_manager.hpp>
#include <caf/cow_tuple.hpp>
#include <caf/detail/scope_guard.hpp>
#include <caf/fused_downstream_manager.hpp>
#include <caf/fwd.hpp>
#include <caf/message.hpp>
#include <caf/sec.hpp>
#include <caf/settings.hpp>
#include <caf/stream_manager.hpp>
#include <caf/stream_slot.hpp>

#include "broker/atoms.hh"
#include "broker/data.hh"
#include "broker/defaults.hh"
#include "broker/detail/assert.hh"
#include "broker/detail/filesystem.hh"
#include "broker/detail/generator_file_writer.hh"
#include "broker/detail/prefix_matcher.hh"
#include "broker/error.hh"
#include "broker/filter_type.hh"
#include "broker/internal_command.hh"
#include "broker/logger.hh"
#include "broker/message.hh"
#include "broker/peer_filter.hh"
#include "broker/status.hh"
#include "broker/topic.hh"

namespace broker::alm {

/// Sets up a configurable stream manager to act as a distribution tree for
/// Broker.
template <class Derived, class PeerId>
class stream_transport : public caf::stream_manager {
public:
  // -- member types -----------------------------------------------------------

  using peer_id_type = PeerId;

  using message_type = generic_node_message<PeerId>;

  using communication_handle_type = caf::actor;

  struct pending_connection {
    caf::stream_slot slot;
    caf::response_promise rp;
  };

  /// Type to store a TTL for messages forwarded to peers.
  using ttl = uint16_t;

  /// Helper trait for defining streaming-related types for local actors
  /// (workers and stores).
  template <class T>
  struct local_trait {
    /// Type of a single element in the stream.
    using element = caf::cow_tuple<topic, T>;

    /// Type of a full batch in the stream.
    using batch = std::vector<element>;

    /// Type of the downstream_manager that broadcasts data to local actors.
    using manager = caf::broadcast_downstream_manager<element, filter_type,
                                                      detail::prefix_matcher>;
  };

  /// Streaming-related types for workers.
  using worker_trait = local_trait<data>;

  /// Streaming-related types for stores.
  using store_trait = local_trait<internal_command>;

  /// Streaming-related types for sources that produce both types of messages.
  struct var_trait {
    using batch = std::vector<typename message_type::value_type>;
  };

  /// Streaming-related types for peers.
  struct peer_trait {
    /// Type of a single element in the stream.
    using element = message_type;

    using batch = std::vector<element>;

    /// Type of the downstream_manager that broadcasts data to local actors.
    using manager = caf::broadcast_downstream_manager<element, peer_filter,
                                                      peer_filter_matcher>;
  };

  /// Composed downstream_manager type for bundled dispatching.
  using downstream_manager_type
    = caf::fused_downstream_manager<typename peer_trait::manager,
                                    typename worker_trait::manager,
                                    typename store_trait::manager>;

  /// Maps actor handles to path IDs.
  using hdl_to_slot_map = std::unordered_map<caf::actor, caf::stream_slot>;

  /// Maps path IDs to actor handles.
  using slot_to_hdl_map = std::unordered_map<caf::stream_slot, caf::actor>;

  // -- constructors, destructors, and assignment operators --------------------

  stream_transport(caf::event_based_actor* self, const filter_type& filter)
    : caf::stream_manager(self), out_(this), remaining_records_(0) {
    continuous(true);
    // TODO: use filter
    using caf::get_or;
    auto& cfg = self->system().config();
    auto meta_dir = get_or(cfg, "broker.recording-directory",
                           defaults::recording_directory);
    if (!meta_dir.empty() && detail::is_directory(meta_dir)) {
      auto file_name = meta_dir + "/messages.dat";
      recorder_ = detail::make_generator_file_writer(file_name);
      if (recorder_ == nullptr) {
        BROKER_WARNING("cannot open recording file" << file_name);
      } else {
        BROKER_DEBUG("opened file for recording:" << file_name);
        remaining_records_ = get_or(cfg, "broker.output-generator-file-cap",
                                    defaults::output_generator_file_cap);
      }
    }
  }

  // -- initialization ---------------------------------------------------------

  template <class... Fs>
  caf::behavior make_behavior(Fs... fs) {
    return {std::move(fs)...};
  }

  // -- properties -------------------------------------------------------------

  caf::event_based_actor* self() noexcept {
    // Our only constructor accepts an event-based actor. Hence, we know for
    // sure that this case is safe, even though the base type stores self_ as a
    // scheduled_actor pointer.
    return static_cast<caf::event_based_actor*>(this->self_);
  }

  auto& pending_connections() noexcept {
    return pending_connections_;
  }

  /// Returns the downstream_manager for peer traffic.
  auto& peer_manager() noexcept {
    return out().template get<typename peer_trait::manager>();
  }

  /// Returns the downstream_manager for worker traffic.
  auto& worker_manager() noexcept {
    return out().template get<typename worker_trait::manager>();
  }

  /// Returns the downstream_manager for data store traffic.
  auto& store_manager() noexcept {
    return out().template get<typename store_trait::manager>();
  }

  // -- streaming helper functions ---------------------------------------------

  void ack_open_success(caf::stream_slot slot,
                        const caf::actor_addr& rebind_from,
                        caf::strong_actor_ptr rebind_to) {
    BROKER_TRACE(BROKER_ARG(slot)
                 << BROKER_ARG(rebind_from) << BROKER_ARG(rebind_to));
    if (rebind_from != rebind_to) {
      BROKER_DEBUG("rebind occurred" << BROKER_ARG(slot)
                                     << BROKER_ARG(rebind_from)
                                     << BROKER_ARG(rebind_to));
      peer_manager().filter(slot).first
        = caf::actor_cast<caf::actor_addr>(rebind_to);
    }
  }

  void ack_open_failure(caf::stream_slot slot,
                        const caf::actor_addr& rebind_from,
                        caf::strong_actor_ptr rebind_to) {
    BROKER_TRACE(BROKER_ARG(slot)
                 << BROKER_ARG(rebind_from) << BROKER_ARG(rebind_to));
    CAF_IGNORE_UNUSED(rebind_from);
    CAF_IGNORE_UNUSED(rebind_to);
    auto i = ostream_to_peer_.find(slot);
    if (i != ostream_to_peer_.end()) {
      auto hdl = i->second;
      remove_peer(hdl, make_error(caf::sec::invalid_stream_state), false,
                  false);
    }
  }

  void push_to_substreams(std::vector<caf::message> xs) {
    // Dispatch on the content of `xs`.
    for (auto& x : xs) {
      if (x.match_elements<topic, data>()) {
        x.force_unshare();
        worker_manager().push(std::move(x.get_mutable_as<topic>(0)),
                              std::move(x.get_mutable_as<data>(1)));
      } else if (x.match_elements<topic, internal_command>()) {
        x.force_unshare();
        store_manager().push(std::move(x.get_mutable_as<topic>(0)),
                             std::move(x.get_mutable_as<internal_command>(1)));
      }
    }
    worker_manager().emit_batches();
    store_manager().emit_batches();
  }

  // -- peer management --------------------------------------------------------

  /// Queries whether `hdl` is a known peer.
  bool connected_to(const caf::actor& hdl) const {
    return hdl_to_ostream_.count(hdl) != 0 || hdl_to_istream_.count(hdl) != 0;
  }

  /// Block peer messages from being handled.  They are buffered until unblocked.
  void block_peer(caf::actor peer) {
    blocked_peers.emplace(std::move(peer));
  }

  /// Unblock peer messages and flush any buffered messages immediately.
  void unblock_peer(caf::actor peer) {
    blocked_peers.erase(peer);
    auto it = blocked_msgs.find(peer);
    if (it == blocked_msgs.end())
      return;
    auto pit = hdl_to_istream_.find(peer);
    if (pit == hdl_to_istream_.end()) {
      blocked_msgs.erase(it);
      BROKER_DEBUG(
        "dropped batches after unblocking peer: path no longer exists" << peer);
      return;
    }
    auto& slot = pit->second;
    auto sap = caf::actor_cast<caf::strong_actor_ptr>(peer);
    for (auto& batch : it->second) {
      BROKER_DEBUG("handle blocked batch" << peer);
      handle_batch(sap, batch);
    }
    blocked_msgs.erase(it);
  }

  /// Disconnects a peer by demand of the user.
  void unpeer(const peer_id_type& peer_id, const caf::actor& hdl) {
    BROKER_TRACE(BROKER_ARG(peer_id) << BROKER_ARG(hdl));
    if (!remove_peer(hdl, caf::none, false, true))
      dref().cannot_remove_peer(peer_id, hdl);
  }

  /// Disconnects a peer by demand of the user.
  void unpeer(const caf::actor& hdl) {
    BROKER_TRACE(BROKER_ARG(hdl));
    if (!hdl)
      return;
    unpeer(hdl.node(), hdl);
  }

  /// Starts the handshake process for a new peering (step #1 in core_actor.cc).
  /// @returns `false` if the peer is already connected, `true` otherwise.
  /// @param peer_hdl Handle to the peering (remote) core actor.
  /// @param peer_filter Filter of our peer.
  /// @param send_own_filter Sends a `(filter, self)` handshake if `true`,
  ///                        `('ok', self)` otherwise.
  /// @pre `current_sender() != nullptr`
  template <bool SendOwnFilter>
  auto start_handshake(const caf::actor& peer_hdl, filter_type peer_filter) {
    BROKER_TRACE(BROKER_ARG(peer_hdl) << BROKER_ARG(peer_filter));
    // Token for static dispatch of add().
    std::integral_constant<bool, SendOwnFilter> send_own_filter_token;
    // Check whether we already send outbound traffic to the peer. Could use
    // `CAF_ASSERT` instead, because this must'nt get called for known peers.
    using result_type = decltype(add(send_own_filter_token, peer_hdl));
    if (hdl_to_ostream_.count(peer_hdl) != 0) {
      BROKER_ERROR("peer already connected");
      return result_type{};
    }
    // Add outbound path to the peer.
    auto slot = add(send_own_filter_token, peer_hdl);
    // Make sure the peer receives the correct traffic.
    out().template assign<typename peer_trait::manager>(slot);
    peer_manager().set_filter(
      slot, std::make_pair(peer_hdl.address(), std::move(peer_filter)));
    // Add bookkeeping state for our new peer.
    add_opath(slot, peer_hdl);
    return slot;
  }

  /// Initiates peering between this peer and `remote_peer`.
  void start_peering(const peer_id_type&, caf::actor remote_peer,
                     caf::response_promise rp) {
    BROKER_TRACE(BROKER_ARG(remote_peer));
    // Sanity checking.
    if (remote_peer == nullptr) {
      rp.deliver(caf::sec::invalid_argument);
      return ;
    }
    // Ignore repeated peering requests without error.
    if (pending_connections().count(remote_peer) > 0
        || connected_to(remote_peer)) {
      rp.deliver(caf::unit);
      return;
    }
    // Create necessary state and send message to remote core.
    pending_connections().emplace(remote_peer,
                                  pending_connection{0, std::move(rp)});
    self()->send(self() * remote_peer, atom::peer::value, dref().filter(),
                 self());
    self()->monitor(remote_peer);
  }

  /// Acknowledges an incoming peering request (step #2/3 in core_actor.cc).
  /// @param peer_hdl Handle to the peering (remote) core actor.
  /// @returns `false` if the peer is already connected, `true` otherwise.
  /// @pre Current message is an `open_stream_msg`.
  void ack_peering(const caf::stream<message_type>& in,
                   const caf::actor& peer_hdl) {
    BROKER_TRACE(BROKER_ARG(peer_hdl));
    // Check whether we already receive inbound traffic from the peer. Could use
    // `CAF_ASSERT` instead, because this must'nt get called for known peers.
    if (hdl_to_istream_.count(peer_hdl) != 0) {
      BROKER_ERROR("peer already connected");
      return;
    }
    // Add inbound path for our peer.
    auto slot = this->add_unchecked_inbound_path(in);
    add_ipath(slot, peer_hdl);
  }

  /// Queries whether we have an outbound path to `hdl`.
  bool has_outbound_path_to(const caf::actor& peer_hdl) {
    return hdl_to_ostream_.count(peer_hdl) != 0;
  }

  /// Queries whether we have an inbound path from `hdl`.
  bool has_inbound_path_from(const caf::actor& peer_hdl) {
    return hdl_to_istream_.count(peer_hdl) != 0;
  }

  /// Removes a peer, aborting any stream to and from that peer.
  bool remove_peer(const caf::actor& hdl, caf::error reason, bool silent,
                   bool graceful_removal) {
    BROKER_TRACE(BROKER_ARG(hdl));
    int performed_erases = 0;
    { // lifetime scope of first iterator pair
      auto e = hdl_to_ostream_.end();
      auto i = hdl_to_ostream_.find(hdl);
      if (i != e) {
        BROKER_DEBUG("remove outbound path to peer:" << hdl);
        ++performed_erases;
        out().remove_path(i->second, reason, silent);
        ostream_to_peer_.erase(i->second);
        hdl_to_ostream_.erase(i);
      }
    }
    { // lifetime scope of second iterator pair
      auto e = hdl_to_istream_.end();
      auto i = hdl_to_istream_.find(hdl);
      if (i != e) {
        BROKER_DEBUG("remove inbound path to peer:" << hdl);
        ++performed_erases;
        this->remove_input_path(i->second, reason, silent);
        istream_to_hdl_.erase(i->second);
        hdl_to_istream_.erase(i);
      }
    }
    if (performed_erases == 0) {
      BROKER_DEBUG("no path was removed for peer:" << hdl);
      return false;
    }
    if (graceful_removal)
      dref().peer_removed(hdl.node(), hdl);
    else
      dref().peer_disconnected(hdl.node(), hdl, reason);
    dref().cache().remove(hdl);
    if (dref().shutting_down() && hdl_to_ostream_.empty()) {
      // Shutdown when the last peer stops listening.
      self()->quit(caf::exit_reason::user_shutdown);
    } else {
      // See whether we can make progress without that peer in the mix.
      this->push();
    }
    return true;
  }

  /// Updates the filter of an existing peer.
  bool update_peer(const caf::actor& hdl, filter_type filter) {
    BROKER_TRACE(BROKER_ARG(hdl) << BROKER_ARG(filter));
    auto e = hdl_to_ostream_.end();
    auto i = hdl_to_ostream_.find(hdl);
    if (i == e) {
      BROKER_DEBUG("cannot update filter on unknown peer");
      return false;
    }
    peer_manager().filter(i->second).second = std::move(filter);
    return true;
  }

  // -- management of worker and storage streams -------------------------------

  /// Adds the sender of the current message as worker by starting an output
  /// stream to it.
  /// @pre `current_sender() != nullptr`
  caf::outbound_stream_slot<typename worker_trait::element>
  add_worker(filter_type filter) {
    BROKER_TRACE(BROKER_ARG(filter));
    auto slot = this->template add_unchecked_outbound_path<
      typename worker_trait::element>();
    if (slot != caf::invalid_stream_slot) {
      out().template assign<typename worker_trait::manager>(slot);
      worker_manager().set_filter(slot, std::move(filter));
    }
    return slot;
  }

  /// Subscribes `self->sender()` to `store_manager()`.
  auto add_sending_store(const filter_type& filter) {
    using element_type = typename store_trait::element;
    using result_type = caf::outbound_stream_slot<element_type>;
    auto slot = add_unchecked_outbound_path<element_type>();
    if (slot != caf::invalid_stream_slot) {
      dref().subscribe(filter);
      out_.template assign<typename store_trait::manager>(slot);
      store_manager().set_filter(slot, filter);
    }
    return result_type{slot};
  }

  /// Subscribes `hdl` to `store_manager()`.
  caf::error add_store(const caf::actor& hdl, const filter_type& filter) {
    using element_type = typename store_trait::element;
    auto slot = add_unchecked_outbound_path<element_type>(hdl);
    if (slot == caf::invalid_stream_slot)
      return caf::sec::cannot_add_downstream;
    dref().subscribe(filter);
    out_.template assign<typename store_trait::manager>(slot);
    store_manager().set_filter(slot, filter);
    return caf::none;
  }

  // -- selectively pushing data into the streams ------------------------------

  /// Pushes data to workers without forwarding it to peers.
  void local_push(data_message x) {
    BROKER_TRACE(BROKER_ARG(x)
                 << BROKER_ARG2("num_paths", worker_manager().num_paths()));
    if (worker_manager().num_paths() > 0) {
      worker_manager().push(std::move(x));
      worker_manager().emit_batches();
    }
  }

  /// Pushes data to stores without forwarding it to peers.
  void local_push(command_message x) {
    BROKER_TRACE(BROKER_ARG(x)
                 << BROKER_ARG2("num_paths", store_manager().num_paths()));
    if (store_manager().num_paths() > 0) {
      store_manager().push(std::move(x));
      store_manager().emit_batches();
    }
  }

  /// Pushes data to peers only without forwarding it to local substreams.
  void remote_push(message_type msg) {
    BROKER_TRACE(BROKER_ARG(msg));
    peer_manager().push(std::move(msg));
    peer_manager().emit_batches();
  }

  using caf::stream_manager::push;

  /// Pushes data to peers and workers.
  void push(data_message msg) {
    BROKER_TRACE(BROKER_ARG(msg));
    remote_push(make_node_message(std::move(msg), dref().options().ttl));
    // local_push(std::move(x), std::move(y));
  }

  /// Pushes data to peers and stores.
  void push(command_message msg) {
    BROKER_TRACE(BROKER_ARG(msg));
    remote_push(make_node_message(std::move(msg), dref().options().ttl));
    // local_push(std::move(x), std::move(y));
  }

  /// Pushes data to peers and stores.
  void push(message_type msg) {
    BROKER_TRACE(BROKER_ARG(msg));
    remote_push(std::move(msg));
  }

  // -- communication that bypasses the streams --------------------------------

  void ship(data_message& msg, const communication_handle_type& hdl) {
    self()->send(hdl, atom::publish::value, atom::local::value, std::move(msg));
  }

  template <class T>
  void ship(T& msg) {
    push(std::move(msg));
  }

  void ship(message_type& msg) {
    push(std::move(msg));
  }

  template <class T>
  void publish(T msg) {
    dref().ship(msg);
  }

  void publish(node_message_content msg) {
    visit([this](auto& x) { dref().ship(x); }, msg);
  }

  // -- overridden member functions of caf::stream_manager ---------------------

  void handle_batch(const caf::strong_actor_ptr& hdl, caf::message& xs) {
    BROKER_TRACE(BROKER_ARG(hdl) << BROKER_ARG(xs));
    // If there's anything in the central buffer at this point, it's stuff that
    // we're sending out ourselves (as opposed to forwarding), so we flush it
    // out to each path's own cache now to make sure the subsequent flush in
    // after_handle_batch doesn't accidentally filter out messages where the
    // outband path of previously-buffered messagesi happens to match the path
    // of the inbound data we are handling here.
    BROKER_ASSERT(peer_manager().selector().active_sender == nullptr);
    auto& d = dref();
    peer_manager().fan_out_flush();
    peer_manager().selector().active_sender = caf::actor_cast<caf::actor_addr>(hdl);
    auto guard = caf::detail::make_scope_guard([this] {
      // Make sure the content of the buffer is pushed to the outbound paths
      // while the sender filter is still active.
      peer_manager().fan_out_flush();
      peer_manager().selector().active_sender = nullptr;
    });
    // Handle received batch.
    if (xs.match_elements<typename peer_trait::batch>()) {
      auto peer_actor = caf::actor_cast<caf::actor>(hdl);
      auto it = blocked_peers.find(peer_actor);
      if (it != blocked_peers.end()) {
        BROKER_DEBUG("buffer batch from blocked peer" << hdl);
        auto& bmsgs = blocked_msgs[peer_actor];
        bmsgs.emplace_back(std::move(xs));
        return;
      }
      auto num_workers = worker_manager().num_paths();
      auto num_stores = store_manager().num_paths();
      BROKER_DEBUG("forward batch from peers;" << BROKER_ARG(num_workers)
                                               << BROKER_ARG(num_stores));
      // Only received from other peers. Extract content for to local workers
      // or stores and then forward to other peers.
      for (auto& msg : xs.get_mutable_as<typename peer_trait::batch>(0)) {
        const topic* t;
        // Dispatch to local workers or stores messages.
        if (is_data_message(msg)) {
          auto& dm = get<data_message>(msg.content);
          t = &get_topic(dm);
          if (num_workers > 0)
            worker_manager().push(dm);
        } else {
          auto& cm = get<command_message>(msg.content);
          t = &get_topic(cm);
          if (num_stores > 0)
            store_manager().push(cm);
        }
        // Check if forwarding is on.
        if (!dref().options().forward)
          continue;
        // Somewhat hacky, but don't forward data store clone messages.
        auto ends_with = [](const std::string& s, const std::string& ending) {
          if (ending.size() > s.size())
            return false;
          return std::equal(ending.rbegin(), ending.rend(), s.rbegin());
        };
        if (ends_with(t->string(), topics::clone_suffix.string()))
          continue;
        // Either decrease TTL if message has one already, or add one.
        if (--msg.ttl == 0) {
          BROKER_WARNING("dropped a message with expired TTL");
          continue;
        }
        // Forward to other peers.
        d.publish(std::move(msg));
      }
      return;
    }
    auto try_publish = [&](auto trait) {
      using batch_type = typename decltype(trait)::batch;
      if (xs.template match_elements<batch_type>()) {
        for (auto& x : xs.template get_mutable_as<batch_type>(0))
          d.publish(x);
        return true;
      }
      return false;
    };
    if (try_publish(worker_trait{}) || try_publish(store_trait{})
        || try_publish(var_trait{}))
      return;
    BROKER_ERROR("unexpected batch:" << deep_to_string(xs));
  }

  void handle(caf::inbound_path* path,
              caf::downstream_msg::batch& batch) override {
    handle_batch(path->hdl, batch.xs);
  }

  void handle(caf::inbound_path* path, caf::downstream_msg::close& x) override {
    BROKER_TRACE(BROKER_ARG(path) << BROKER_ARG(x));
    auto slot = path->slots.receiver;
    remove_cb(slot, istream_to_hdl_, hdl_to_istream_, hdl_to_ostream_, caf::none);
  }

  void handle(caf::inbound_path* path,
              caf::downstream_msg::forced_close& x) override {
    BROKER_TRACE(BROKER_ARG(path) << BROKER_ARG(x));
    auto slot = path->slots.receiver;
    remove_cb(slot, istream_to_hdl_, hdl_to_istream_, hdl_to_ostream_,
              std::move(x.reason));
  }

  void handle(caf::stream_slots slots, caf::upstream_msg::drop& x) override {
    BROKER_TRACE(BROKER_ARG(slots) << BROKER_ARG(x));
    caf::stream_manager::handle(slots, x);
  }

  void handle(caf::stream_slots slots,
              caf::upstream_msg::forced_drop& x) override {
    BROKER_TRACE(BROKER_ARG(slots) << BROKER_ARG(x));
    auto slot = slots.receiver;
    if (out_.remove_path(slots.receiver, x.reason, true))
      remove_cb(slot, ostream_to_peer_, hdl_to_ostream_, hdl_to_istream_,
                std::move(x.reason));
  }

  bool handle(caf::stream_slots slots,
              caf::upstream_msg::ack_open& x) override {
    BROKER_TRACE(BROKER_ARG(slots) << BROKER_ARG(x));
    auto rebind_from = x.rebind_from;
    auto rebind_to = x.rebind_to;
    if (caf::stream_manager::handle(slots, x)) {
      ack_open_success(slots.receiver, rebind_from, rebind_to);
      return true;
    }
    ack_open_failure(slots.receiver, rebind_from, rebind_to);
    return false;
  }

  bool done() const override {
    return !continuous() && pending_handshakes_ == 0 && inbound_paths_.empty()
           && out_.clean();
  }

  bool idle() const noexcept override {
    // Same as `stream_stage<...>`::idle().
    return out_.stalled() || (out_.clean() && this->inbound_paths_idle());
  }

  downstream_manager_type& out() override {
    return out_;
  }

  /// Applies `f` to each peer.
  template <class F>
  void for_each_peer(F f) {
    // visit all peers that have at least one path still connected
    auto peers = peer_handles();
    std::for_each(peers.begin(), peers.end(), std::move(f));
  }

  /// Returns all known peers.
  auto peer_handles() {
    std::vector<caf::actor> peers;
    for (auto& kvp : hdl_to_ostream_)
      peers.emplace_back(kvp.first);
    for (auto& kvp : hdl_to_istream_)
      peers.emplace_back(kvp.first);
    auto b = peers.begin();
    auto e = peers.end();
    std::sort(b, e);
    auto p = std::unique(b, e);
    if (p != e)
      peers.erase(p, e);
    return peers;
  }

  /// Finds the first peer handle that satisfies the predicate.
  template <class Predicate>
  caf::actor find_output_peer_hdl(Predicate pred) {
    for (auto& kvp : hdl_to_ostream_)
      if (pred(kvp.first))
        return kvp.first;
    return nullptr;
  }

  /// Applies `f` to each filter.
  template <class F>
  void for_each_filter(F f) {
    for (auto& kvp : peer_manager().states()) {
      f(kvp.second.filter);
    }
  }

  // -- fallback implementations to enable forwarding chains -------------------

  void subscribe(const filter_type&) {
    // nop
  }

  // -- callbacks --------------------------------------------------------------

  /// Called whenever new data for local subscribers became available.
  /// @param msg Data or command message, either received by peers or generated
  ///            from a local publisher.
  /// @tparam T Either ::data_message or ::command_message.
  template <class T>
  void ship_locally(T msg) {
    local_push(std::move(msg));
  }

  /// Called whenever this peer established a new connection.
  /// @param peer_id ID of the newly connected peer.
  /// @param hdl Communication handle for exchanging messages with the new peer.
  ///            The handle is default-constructed if no direct connection
  ///            exists (yet).
  /// @note The new peer gets stored in the routing table *before* calling this
  ///       member function.
  void peer_connected([[maybe_unused]] const peer_id_type& peer_id,
                      [[maybe_unused]] const communication_handle_type& hdl) {
    // nop
  }

  /// Called whenever this peer lost a connection to a remote peer.
  /// @param peer_id ID of the disconnected peer.
  /// @param hdl Communication handle of the disconnected peer.
  /// @param reason None if we closed the connection gracefully, otherwise
  ///               contains the transport-specific error code.
  void peer_disconnected([[maybe_unused]] const peer_id_type& peer_id,
                         [[maybe_unused]] const communication_handle_type& hdl,
                         [[maybe_unused]] const error& reason) {
    // nop
  }

  /// Called whenever this peer removed a direct connection to a remote peer.
  /// @param peer_id ID of the removed peer.
  /// @param hdl Communication handle of the removed peer.
  void peer_removed([[maybe_unused]] const peer_id_type& peer_id,
                    [[maybe_unused]] const communication_handle_type& hdl) {
    // nop
  }

  /// Called whenever the user tried to unpeer from an unconnected peer.
  /// @param addr Host information for the unconnected peer.
  void cannot_remove_peer([[maybe_unused]] const network_info& addr) {
    // nop
  }

  /// Called whenever the user tried to unpeer from an unconnected peer.
  /// @param peer_id ID of the unconnected peer.
  /// @param hdl Communication handle of the unconnected peer (may be null).
  void
  cannot_remove_peer([[maybe_unused]] const peer_id_type& peer_id,
                     [[maybe_unused]] const communication_handle_type& hdl) {
    // nop
  }

  /// Called whenever establishing a connection to a remote peer failed.
  /// @param addr Host information for the unavailable peer.
  void peer_unavailable([[maybe_unused]] const network_info& addr) {
    // nop
  }

  /// Called whenever we could obtain a connection handle to a remote peer but
  /// received a `down_msg` before completing the handshake.
  /// @param peer_id ID of the unavailable peer.
  /// @param hdl Communication handle of the unavailable peer.
  /// @param reason Exit reason of the unavailable peer.
  void peer_unavailable([[maybe_unused]] const peer_id_type& peer_id,
                        [[maybe_unused]] const communication_handle_type& hdl,
                        [[maybe_unused]] const error& reason) {
    // nop
  }

protected:
  /// Returns the initial TTL value when publishing data.
  ttl initial_ttl() {
    return static_cast<ttl>(dref().options().ttl);
  }

  /// Adds entries to `hdl_to_istream_` and `istream_to_hdl_`.
  void add_ipath(caf::stream_slot slot, const caf::actor& peer_hdl) {
    BROKER_TRACE(BROKER_ARG(slot) << BROKER_ARG(peer_hdl));
    if (slot == caf::invalid_stream_slot) {
      BROKER_ERROR("tried to add an invalid inbound path");
      return;
    }
    if (!istream_to_hdl_.emplace(slot, peer_hdl).second) {
      BROKER_ERROR("ipath_to_peer entry already exists");
      return;
    }
    if (!hdl_to_istream_.emplace(peer_hdl, slot).second) {
      BROKER_ERROR("peer_to_ipath entry already exists");
      return;
    }
  }

  /// Adds entries to `hdl_to_ostream_` and `ostream_to_peer_`.
  void add_opath(caf::stream_slot slot, const caf::actor& peer_hdl) {
    BROKER_TRACE(BROKER_ARG(slot) << BROKER_ARG(peer_hdl));
    if (slot == caf::invalid_stream_slot) {
      BROKER_ERROR("tried to add an invalid outbound path");
      return;
    }
    if (!ostream_to_peer_.emplace(slot, peer_hdl).second) {
      BROKER_ERROR("opath_to_peer entry already exists");
      return;
    }
    if (!hdl_to_ostream_.emplace(peer_hdl, slot).second) {
      BROKER_ERROR("peer_to_opath entry already exists");
      return;
    }
  }

  /// Path `slot` in `xs` was dropped or closed. Removes the entry in `xs` as
  /// well as the associated entry in `ys`. Also removes the entries from `as`
  /// and `bs` if `reason` is not default constructed. Calls `remove_peer` if
  /// no entry for a peer exists afterwards.
  void remove_cb(caf::stream_slot slot, slot_to_hdl_map& xs,
                 hdl_to_slot_map& ys, hdl_to_slot_map& zs, caf::error reason) {
    BROKER_TRACE(BROKER_ARG(slot));
    auto i = xs.find(slot);
    if (i == xs.end()) {
      BROKER_DEBUG("no entry in xs found for slot" << slot);
      return;
    }
    auto peer_hdl = i->second;
    remove_peer(peer_hdl, std::move(reason), true, false);
  }

  /// Sends a handshake with filter in step #1.
  auto add(std::true_type send_own_filter, const caf::actor& hdl) {
    auto xs
      = std::make_tuple(dref().filter(), caf::actor_cast<caf::actor>(self()));
    return this->template add_unchecked_outbound_path<node_message>(
      hdl, std::move(xs));
  }

  /// Sends a handshake with 'ok' in step #2.
  auto add(std::false_type send_own_filter, const caf::actor& hdl) {
#if CAF_VERSION < 1800
    caf::atom_value ok = atom::ok_v;
    auto xs = std::make_tuple(ok, caf::actor_cast<caf::actor>(self()));
#else
    auto xs = std::make_tuple(atom::ok_v, caf::actor_cast<caf::actor>(self()));
#endif
    return this->template add_unchecked_outbound_path<node_message>(
      hdl, std::move(xs));
  }

  /// Organizes downstream communication to peers as well as local subscribers.
  downstream_manager_type out_;

  /// Maps peer handles to output path IDs.
  hdl_to_slot_map hdl_to_ostream_;

  /// Maps output path IDs to peer handles.
  slot_to_hdl_map ostream_to_peer_;

  /// Maps peer handles to input path IDs.
  hdl_to_slot_map hdl_to_istream_;

  /// Maps input path IDs to peer handles.
  slot_to_hdl_map istream_to_hdl_;

  /// Peers that are currently blocked (messages buffered until unblocked).
  std::unordered_set<caf::actor> blocked_peers;

  /// Messages that are currently buffered.
  std::unordered_map<caf::actor, std::vector<caf::message>> blocked_msgs;

  /// Maps pending peer handles to output IDs. An invalid stream ID indicates
  /// that only "step #0" was performed so far. An invalid stream ID corresponds
  /// to `peer_status::connecting` and a valid stream ID cooresponds to
  /// `peer_status::connected`. The status for a given handle `x` is
  /// `peer_status::peered` if `governor->has_peer(x)` returns true.
  std::unordered_map<caf::actor, pending_connection> pending_connections_;

  /// Helper for recording meta data of published messages.
  detail::generator_file_writer_ptr recorder_;

  /// Counts down when using a `recorder_` to cap maximum file entries.
  size_t remaining_records_;

private:
  Derived& dref() {
    return static_cast<Derived&>(*this);
  }
};

} // namespace broker::alm
