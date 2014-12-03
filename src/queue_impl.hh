#ifndef BROKER_QUEUE_IMPL_HH
#define BROKER_QUEUE_IMPL_HH

#include "broker/queue.hh"
#include "util/flare.hh"
#include "util/queue.hh"
#include <caf/spawn.hpp>
#include <caf/send.hpp>

namespace broker {

template <class T>
class queue<T>::impl {
public:

	impl()
		{
		util::flare f;
		fd = f.fd();
		actor = caf::spawn<broker::util::queue<decltype(caf::on<T>()),
		                                       T>>(std::move(f));
		self->planned_exit_reason(caf::exit_reason::user_defined);
		actor->link_to(self);
		}

	int fd;
	caf::scoped_actor self;
	caf::actor actor;
};

} // namespace broker

#endif // BROKER_QUEUE_IMPL_HH