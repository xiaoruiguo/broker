#define SUITE master

#include "broker/store.hh"

#include "test.hh"

#include <regex>

#include <caf/test/io_dsl.hpp>

#include "broker/atoms.hh"
#include "broker/backend.hh"
#include "broker/data.hh"
#include "broker/endpoint.hh"
#include "broker/error.hh"
#include "broker/filter_type.hh"
#include "broker/internal_command.hh"
#include "broker/store_event.hh"
#include "broker/topic.hh"

using std::cout;
using std::endl;
using std::string;

using namespace caf;
using namespace broker;
using namespace broker::detail;

namespace {

using string_list = std::vector<string>;

class pattern_list {
public:
  explicit pattern_list(std::initializer_list<const char*> strings) {
    patterns_.reserve(strings.size());
    for (auto str : strings)
      patterns_.emplace_back(str, str);
  }

  pattern_list(const pattern_list&) = default;

  auto begin() const {
    return patterns_.begin();
  }

  auto end() const {
    return patterns_.end();
  }

private:
  std::vector<std::pair<std::string, std::regex>> patterns_;
};

std::string to_string(const pattern_list& xs) {
  auto i = xs.begin();
  auto e = xs.end();
  if (i == e)
    return "[]";
  std::string result = "[";
  auto append_quoted = [&](const std::string& x) {
    result += '"';
    result += x;
    result += '"';
  };
  append_quoted(i->first);
  for (++i; i != e; ++i) {
    result += ", ";
    append_quoted(i->first);
  }
  result += ']';
  return result;
}

bool operator==(const string_list& xs, const pattern_list& ys) {
  auto matches = [](const std::string& x, const auto& y) {
    return std::regex_match(x, y.second);
  };
  return std::equal(xs.begin(), xs.end(), ys.begin(), ys.end(), matches);
}

struct fixture : base_fixture {
  string_list log;
  caf::actor logger;

  fixture() {
    logger = ep.subscribe_nosync(
      // Topics.
      {topics::store_events},
      // Init.
      [](caf::unit_t&) {},
      // Consume.
      [this](caf::unit_t&, data_message msg) {
        auto content = get_data(msg);
        if (auto insert = store_event::insert::make(content))
          log.emplace_back(to_string(insert));
        else if (auto update = store_event::update::make(content))
          log.emplace_back(to_string(update));
        else if (auto erase = store_event::erase::make(content))
          log.emplace_back(to_string(erase));
        else
          FAIL("unknown event: " << to_string(content));
      },
      // Cleanup.
      [](caf::unit_t&) {});
  }

  ~fixture() {
    anon_send_exit(logger, exit_reason::user_shutdown);
  }
};

} // namespace

CAF_TEST_FIXTURE_SCOPE(local_store_master, fixture)

CAF_TEST(local_master) {
  auto core = ep.core();
  run();
  sched.inline_next_enqueue(); // ep.attach talks to the core (blocking)
  // ep.attach sends a message to the core that will then spawn a new master
  auto expected_ds = ep.attach_master("foo", backend::memory);
  CAF_REQUIRE(expected_ds.engaged());
  auto& ds = *expected_ds;
  MESSAGE(ds.frontend_id());
  auto ms = ds.frontend();
  // the core adds the master immediately to the topic and sends a stream
  // handshake
  run();
  // test putting something into the store
  ds.put("hello", "world");
  run();
  // read back what we have written
  sched.inline_next_enqueue(); // ds.get talks to the master_actor (blocking)
  CAF_CHECK_EQUAL(value_of(ds.get("hello")), data{"world"});
  // check the name of the master
  sched.inline_next_enqueue(); // ds.name talks to the master_actor (blocking)
  auto n = ds.name();
  CAF_CHECK_EQUAL(n, "foo");
  // send put command to the master's topic
  anon_send(core, atom::publish_v, atom::local_v,
            make_command_message(
              n / topics::master_suffix,
              make_internal_command<put_command>("hello", "universe")));
  run();
  // read back what we have written
  sched.inline_next_enqueue(); // ds.get talks to the master_actor (blocking)
  CAF_CHECK_EQUAL(value_of(ds.get("hello")), data{"universe"});
  ds.clear();
  run();
  sched.inline_next_enqueue();
  CAF_CHECK_EQUAL(error_of(ds.get("hello")), caf::error{ec::no_such_key});
  // check log
  CHECK_EQUAL(log, pattern_list({
                     "insert\\(foo, hello, world, none, .+\\)",
                     "update\\(foo, hello, world, universe, none, .+\\)",
                     "erase\\(foo, hello, .+\\)",
                   }));
  // done
  anon_send_exit(core, exit_reason::user_shutdown);
}

CAF_TEST_FIXTURE_SCOPE_END()

CAF_TEST_FIXTURE_SCOPE(store_master, point_to_point_fixture<fixture>)

CAF_TEST(master_with_clone) {
  // --- phase 1: get state from fixtures and initialize cores -----------------
  auto core1 = earth.ep.core();
  auto core2 = mars.ep.core();
  auto forward_stream_traffic = [&] {
    while (earth.mpx.try_exec_runnable() || mars.mpx.try_exec_runnable()
           || earth.mpx.read_data() || mars.mpx.read_data()) {
      // rince and repeat
    }
  };
  anon_send(core1, atom::no_events_v);
  anon_send(core2, atom::no_events_v);
  // --- phase 2: connect earth and mars at CAF level --------------------------
  // Prepare publish and remote_actor calls.
  CAF_MESSAGE("prepare connections on earth and mars");
  prepare_connection(mars, earth, "mars", 8080u);
  // Run any initialization code.
  exec_all();
  // Tell mars to listen for peers.
  CAF_MESSAGE("publish core on mars");
  mars.sched.inline_next_enqueue(); // listen() calls middleman().publish()
  auto res = mars.ep.listen("", 8080u);
  CAF_CHECK_EQUAL(res, 8080u);
  exec_all();
  // Establish connection between mars and earth before peering in order to
  // connect the streaming parts of CAF before we go into Broker code.
  CAF_MESSAGE("connect mars and earth");
  auto core2_proxy = earth.remote_actor("mars", 8080u);
  exec_all();
  // --- phase 4: attach a master on earth -------------------------------------
  CAF_MESSAGE("attach a master on earth");
  earth.sched.inline_next_enqueue();
  auto expected_ds_earth = earth.ep.attach_master("foo", backend::memory);
  if (!expected_ds_earth)
    CAF_FAIL(
      "could not attach master: " << to_string(expected_ds_earth.error()));
  auto& ds_earth = *expected_ds_earth;
  auto ms_earth = ds_earth.frontend();
  // the core adds the master immediately to the topic and sends a stream
  // handshake
  exec_all(); // skip handshake
  // Store some test data in the master.
  expected_ds_earth->put("test", 123);
  expect_on(earth, (atom::local, internal_command), from(_).to(ms_earth));
  exec_all();
  earth.sched.inline_next_enqueue(); // .get talks to the master
  CAF_CHECK_EQUAL(value_of(ds_earth.get("test")), data{123});
  // --- phase 5: peer from earth to mars --------------------------------------
  auto foo_master = "foo" / topics::master_suffix;
  // Initiate handshake between core1 and core2.
  earth.self->send(core1, atom::peer_v, core2_proxy);
  expect_on(earth, (atom::peer, actor),
            from(earth.self).to(core1).with(_, core2_proxy));
  // Step #1: core1  --->    ('peer', filter_type)    ---> core2
  forward_stream_traffic();
  expect_on(mars, (atom::peer, filter_type, actor),
            from(_).to(core2).with(_, filter_type{foo_master}, _));
  // Step #2: core1  <---   (open_stream_msg)   <--- core2
  forward_stream_traffic();
  expect_on(earth, (open_stream_msg), from(_).to(core1));
  // Step #3: core1  --->   (open_stream_msg)   ---> core2
  //          core1  ---> (upstream_msg::ack_open) ---> core2
  forward_stream_traffic();
  expect_on(mars, (open_stream_msg), from(_).to(core2));
  expect_on(mars, (upstream_msg::ack_open), from(_).to(core2));
  // Step #4: core1  <--- (upstream_msg::ack_open) <--- core2
  forward_stream_traffic();
  expect_on(earth, (upstream_msg::ack_open), from(_).to(core1));
  // Make sure there is no communication pending at this point.
  exec_all();
  // --- phase 7: attach a clone on mars ---------------------------------------
  mars.sched.inline_next_enqueue();
  CAF_MESSAGE("attach a clone on mars");
  mars.sched.inline_next_enqueue();
  auto expected_ds_mars = mars.ep.attach_clone("foo");
  CAF_REQUIRE(expected_ds_mars.engaged());
  auto& ds_mars = *expected_ds_mars;
  auto& ms_mars = ds_mars.frontend();
  // the core adds the clone immediately to the topic and sends a stream
  // handshake
  auto foo_clone = "foo" / topics::clone_suffix;
  expect_on(mars, (open_stream_msg), from(_).to(ms_mars));
  expect_on(mars, (upstream_msg::ack_open),
            from(ms_mars).to(core2).with(_, _, _, false));
  // the core also updates its filter on all peers ...
  network_traffic();
  expect_on(earth, (atom::update, filter_type),
            from(_).to(core1).with(_, filter_type{foo_clone}));
  // -- phase 8: run it all & check results ------------------------------------
  exec_all();
  CAF_MESSAGE("put 'user' -> 'neverlord'");
  ds_mars.put("user", "neverlord");
  expect_on(mars, (atom::local, internal_command),
            from(_).to(ds_mars.frontend()));
  expect_on(mars, (atom::publish, command_message), from(_).to(mars.ep.core()));
  exec_all();
  earth.sched.inline_next_enqueue(); // .get talks to the master
  CAF_CHECK_EQUAL(value_of(ds_earth.get("user")), data{"neverlord"});
  mars.sched.inline_next_enqueue(); // .get talks to the master
  CAF_CHECK_EQUAL(value_of(ds_mars.get("test")), data{123});
  mars.sched.inline_next_enqueue(); // .get talks to the master
  CAF_CHECK_EQUAL(value_of(ds_mars.get("user")), data{"neverlord"});
  // done
  anon_send_exit(earth.ep.core(), exit_reason::user_shutdown);
  anon_send_exit(mars.ep.core(), exit_reason::user_shutdown);
  exec_all();
  // check log
  CHECK_EQUAL(mars.log, earth.log);
  CHECK_EQUAL(mars.log, pattern_list({
                          "insert\\(foo, test, 123, none, .+\\)",
                          "insert\\(foo, user, neverlord, none, .+\\)",
                        }));
}

CAF_TEST_FIXTURE_SCOPE_END()
