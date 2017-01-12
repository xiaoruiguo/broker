#include <poll.h>

#include "broker/broker.hh"

#define SUITE multihop
#include "test.hpp"

// A test of "event" style messages -- vectors of data w/ first element
// being the event name.

using namespace broker;

using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

bool is_ready(blocking_endpoint& e, seconds secs = seconds::zero()) {
  auto fd = e.mailbox().descriptor();
  pollfd p = {fd, POLLIN, {}};
  auto n = ::poll(&p, 1, secs.count() * 1000);
  if (n < 0)
    std::terminate();
  return n == 1 && p.revents & POLLIN;
}

} // namespace <anonymous>

TEST(1 chain of nodes) {
  /* Overlay configuration
   * n0 [a]
   * |
   * n1 [b]
   * |  
   * n2 [c]
   */

  // init debugging/reporting 
  //broker::report::init();

  MESSAGE("spawning endpoints");
  context ctx;
  auto n0 = ctx.spawn<blocking>();
  auto n1 = ctx.spawn<blocking+routable>();
  auto n2 = ctx.spawn<blocking>();

  MESSAGE("connecting peers");
  n0.peer(n1);
  n0.receive([](const status& s) { CHECK(s == peer_added); });
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n1.peer(n2);
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n2.receive([](const status& s) { CHECK(s == peer_added); });

  CHECK_EQUAL(n0.peers().size(), 1u);
  CHECK_EQUAL(n1.peers().size(), 2u);
  CHECK_EQUAL(n2.peers().size(), 1u);
  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());

  MESSAGE("propagating subscriptions");
  n0.subscribe("a");
  n1.subscribe("b");
  n2.subscribe("c");
  std::this_thread::sleep_for(milliseconds{100});

  MESSAGE("Sending n2 -> n0");
  for(int i = 0; i < 2; i++)
    n2.publish("a", "ping");
  for(int i = 0; i < 2; i++) {
    n0.receive([](const topic& t, const data& d) {
      CHECK_EQUAL(t, "a");
      CHECK_EQUAL(d, data{"ping"});
    });
  }

  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());

  MESSAGE("Sending n0 -> n2");
  n0.publish("c", "pong");
  n2.receive([](const topic& t, const data& d) {
    CHECK_EQUAL(t, "c");
    CHECK_EQUAL(d, data{"pong"});
  });

  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());
}

TEST(2 tree) {

  /* Overlay configuration
   *       n0 [a]
   *       |
   *       n1 [b]
   *      /  \
   * [c] n2  n3 [b]
   *         |
   *         n4 [d]
   */

  MESSAGE("spawning endpoints");
  context ctx;
  auto n0 = ctx.spawn<blocking>();
  auto n1 = ctx.spawn<blocking+routable>();
  auto n2 = ctx.spawn<blocking>();
  auto n3 = ctx.spawn<blocking+routable>();
  auto n4 = ctx.spawn<blocking>();

  MESSAGE("connecting peers");
  n0.peer(n1);
  n0.receive([](const status& s) { CHECK(s == peer_added); });
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n1.peer(n2);
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n2.receive([](const status& s) { CHECK(s == peer_added); });
  n1.peer(n3);
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n3.receive([](const status& s) { CHECK(s == peer_added); });
  n3.peer(n4);
  n3.receive([](const status& s) { CHECK(s == peer_added); });
  n4.receive([](const status& s) { CHECK(s == peer_added); });
  CHECK_EQUAL(n0.peers().size(), 1u);
  CHECK_EQUAL(n1.peers().size(), 3u);
  CHECK_EQUAL(n2.peers().size(), 1u);
  CHECK_EQUAL(n3.peers().size(), 2u);
  CHECK_EQUAL(n4.peers().size(), 1u);
  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());
  CHECK(n3.mailbox().empty());
  CHECK(n4.mailbox().empty());

  MESSAGE("propagating subscriptions");
  n0.subscribe("a");
  n1.subscribe("b");
  n2.subscribe("c");
  n3.subscribe("b");
  n4.subscribe("d");
  std::this_thread::sleep_for(milliseconds{100});

  MESSAGE("Sending n2 -> n0");
  n2.publish("a", "ping");
  n0.receive([](const topic& t, const data& d) {CHECK_EQUAL(t, "a");});
  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());
  CHECK(n3.mailbox().empty());
  CHECK(n4.mailbox().empty());

  MESSAGE("Sending n0 -> n4");
  n0.publish("d", "pong");
  n4.receive([](const topic& t, const data& d) {CHECK_EQUAL(t, "d");});
  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());
  CHECK(n3.mailbox().empty());
  CHECK(n4.mailbox().empty());
}

// legacy broker: multihop3 and multihop4
TEST(3 Unpeering and Overlay Partitioning) {

  /* Overlay configuration
   *       n0 [a]
   *       |
   *       n1 [b]
   *      /  \
   * [c] n2  n3 [b]
   *         |
   *         n4 [d]
   *         |
   *         n5 [e]
   * 
   * 1. n0 sends to n5
   * 2. n5 replies to n0 and n2
   * 3. n3 unpeers from n1, which partitions overlay
   * 4. n0 publishes e, which has no subscriber in first overlay
   */

  MESSAGE("spawning endpoints");
  context ctx;
  auto n0 = ctx.spawn<blocking>();
  auto n1 = ctx.spawn<blocking+routable>();
  auto n2 = ctx.spawn<blocking>();
  auto n3 = ctx.spawn<blocking+routable>();
  auto n4 = ctx.spawn<blocking+routable>();
  auto n5 = ctx.spawn<blocking>();

  MESSAGE("connecting peers");
  n0.peer(n1);
  n0.receive([](const status& s) { CHECK(s == peer_added); });
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n1.peer(n2);
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n2.receive([](const status& s) { CHECK(s == peer_added); });
  n1.peer(n3);
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n3.receive([](const status& s) { CHECK(s == peer_added); });
  n3.peer(n4);
  n3.receive([](const status& s) { CHECK(s == peer_added); });
  n4.receive([](const status& s) { CHECK(s == peer_added); });
  n4.peer(n5);
  n4.receive([](const status& s) { CHECK(s == peer_added); });
  n5.receive([](const status& s) { CHECK(s == peer_added); });
  CHECK_EQUAL(n0.peers().size(), 1u);
  CHECK_EQUAL(n1.peers().size(), 3u);
  CHECK_EQUAL(n2.peers().size(), 1u);
  CHECK_EQUAL(n3.peers().size(), 2u);
  CHECK_EQUAL(n4.peers().size(), 2u);
  CHECK_EQUAL(n5.peers().size(), 1u);
  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());
  CHECK(n3.mailbox().empty());
  CHECK(n4.mailbox().empty());
  CHECK(n5.mailbox().empty());

  MESSAGE("propagating subscriptions");
  n0.subscribe("a");
  n1.subscribe("b");
  n2.subscribe("c");
  n3.subscribe("b");
  n4.subscribe("d");
  n5.subscribe("e");
  std::this_thread::sleep_for(milliseconds{100});

  MESSAGE("Sending n0 -> n3 and n0 -> n5");
  n0.publish("b", "ping");
  n0.publish("e", "ping");
  n1.receive([](const topic& t, const data& d) {CHECK_EQUAL(t, "b");});
  n3.receive([](const topic& t, const data& d) {CHECK_EQUAL(t, "b");});
  n5.receive([](const topic& t, const data& d) {CHECK_EQUAL(t, "e");});
  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());
  CHECK(n3.mailbox().empty());
  CHECK(n4.mailbox().empty());
  CHECK(n5.mailbox().empty());

  MESSAGE("Sending n3 -> n0");
  n3.publish("a", "pong");
  n0.receive([](const topic& t, const data& d) {CHECK_EQUAL(t, "a");});
  MESSAGE("n0 " << n0.mailbox().count(99));
  MESSAGE("n1 " << n1.mailbox().count(99));
  MESSAGE("n2 " << n2.mailbox().count(99));
  MESSAGE("n3 " << n3.mailbox().count(99));
  MESSAGE("n4 " << n4.mailbox().count(99));
  MESSAGE("n5 " << n5.mailbox().count(99));
  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());
  CHECK(n3.mailbox().empty());
  CHECK(n4.mailbox().empty());
  CHECK(n5.mailbox().empty());
  std::this_thread::sleep_for(milliseconds{100});

  MESSAGE("Unpeering n3 and n1");
  n3.unpeer(n1);
  std::this_thread::sleep_for(milliseconds{100});
  MESSAGE("Sending n0 -> n1, n3");
  n0.publish("b", "ping");
  n1.receive([](const topic& t, const data& d) {MESSAGE("n1 topic " << t);});
  n3.receive([](const topic& t, const data& d) {MESSAGE("n3 topic " << t);});
  n1.receive([](const topic& t, const data& d) {CHECK_EQUAL(t, "b");});
  MESSAGE("n0 " << n0.mailbox().count(99));
  MESSAGE("n1 " << n1.mailbox().count(99));
  MESSAGE("n2 " << n2.mailbox().count(99));
  MESSAGE("n3 " << n3.mailbox().count(99));
  MESSAGE("n4 " << n4.mailbox().count(99));
  MESSAGE("n5 " << n5.mailbox().count(99));
  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());
  CHECK(n3.mailbox().empty());
  CHECK(n4.mailbox().empty());
  CHECK(n5.mailbox().empty());

  MESSAGE("Sending n0 -> n5");
  n0.publish("e", "ping");
  std::this_thread::sleep_for(milliseconds{100});
  MESSAGE("n0 " << n0.mailbox().count(99));
  MESSAGE("n1 " << n1.mailbox().count(99));
  MESSAGE("n2 " << n2.mailbox().count(99));
  MESSAGE("n3 " << n3.mailbox().count(99));
  MESSAGE("n4 " << n4.mailbox().count(99));
  MESSAGE("n5 " << n5.mailbox().count(99));
  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());
  CHECK(n3.mailbox().empty());
  CHECK(n4.mailbox().empty());
  CHECK(n5.mailbox().empty());
}

// legacy broker: multihop3 and multihop4
TEST(4 Bro Standard Cluster Setup) {
   /**
    * Overlay configuration as in 
    * a typical bro cluster setting
    * 
    * ---------- 
    * |        |          
    * | -------n0[b]------ 
    * | |	      |        | 
    * | |  ---n1[a]---   | 
    * | |  |		|    |   | 
    * | --n2[a] |  n3[a]-- 
    * |	   |		|	   |     
    * |    ---n4[a]---     
    * |	        |          
    * ----------- 
    * 
    * n0 (broctl) is connected to all cluster nodes
    * n0 publishes a message for topic a
    */

  MESSAGE("spawning endpoints");
  context ctx;
  auto n0 = ctx.spawn<blocking>();
  auto n1 = ctx.spawn<blocking>();
  auto n2 = ctx.spawn<blocking>();
  auto n3 = ctx.spawn<blocking>();
  auto n4 = ctx.spawn<blocking>();

  MESSAGE("connecting peers");
  n0.peer(n1);
  n0.receive([](const status& s) { CHECK(s == peer_added); });
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n0.peer(n2);
  n0.receive([](const status& s) { CHECK(s == peer_added); });
  n2.receive([](const status& s) { CHECK(s == peer_added); });
  n0.peer(n3);
  n0.receive([](const status& s) { CHECK(s == peer_added); });
  n3.receive([](const status& s) { CHECK(s == peer_added); });
  n0.peer(n4);
  n0.receive([](const status& s) { CHECK(s == peer_added); });
  n4.receive([](const status& s) { CHECK(s == peer_added); });
  n1.peer(n2);
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n2.receive([](const status& s) { CHECK(s == peer_added); });
  n1.peer(n3);
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n3.receive([](const status& s) { CHECK(s == peer_added); });
  n1.peer(n4);
  n1.receive([](const status& s) { CHECK(s == peer_added); });
  n4.receive([](const status& s) { CHECK(s == peer_added); });
  n2.peer(n4);
  n2.receive([](const status& s) { CHECK(s == peer_added); });
  n4.receive([](const status& s) { CHECK(s == peer_added); });
  n3.peer(n4);
  n3.receive([](const status& s) { CHECK(s == peer_added); });
  n4.receive([](const status& s) { CHECK(s == peer_added); });
  CHECK_EQUAL(n0.peers().size(), 4u);
  CHECK_EQUAL(n1.peers().size(), 4u);
  CHECK_EQUAL(n2.peers().size(), 3u);
  CHECK_EQUAL(n3.peers().size(), 3u);
  CHECK_EQUAL(n4.peers().size(), 4u);
  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());
  CHECK(n3.mailbox().empty());
  CHECK(n4.mailbox().empty());

  MESSAGE("propagating subscriptions");
  n0.subscribe("b");
  n1.subscribe("a");
  n2.subscribe("a");
  n3.subscribe("a");
  n4.subscribe("a");
  std::this_thread::sleep_for(milliseconds{100});

  MESSAGE("Broadcasting message from n0 to all others");
  n0.publish("a", "ping");
  std::this_thread::sleep_for(milliseconds{200});
  MESSAGE("n0 " << n0.mailbox().count(99999));
  MESSAGE("n1 " << n1.mailbox().count(99999));
  MESSAGE("n2 " << n2.mailbox().count(99999));
  MESSAGE("n3 " << n3.mailbox().count(99999));
  MESSAGE("n4 " << n4.mailbox().count(99999));
  n1.receive([](const topic& t, const data& d) {CHECK_EQUAL(t, "a");});
  n2.receive([](const topic& t, const data& d) {CHECK_EQUAL(t, "a");});
  n3.receive([](const topic& t, const data& d) {CHECK_EQUAL(t, "a");});
  n4.receive([](const topic& t, const data& d) {CHECK_EQUAL(t, "a");});
  std::this_thread::sleep_for(milliseconds{100});
  CHECK(n0.mailbox().empty());
  CHECK(n1.mailbox().empty());
  CHECK(n2.mailbox().empty());
  CHECK(n3.mailbox().empty());
  CHECK(n4.mailbox().empty());
}