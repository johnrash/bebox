/*
 * BeBox emulator - user mode networking through libslirp.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * libslirp (BSD license) is the network stack QEMU uses for its "user"
 * networking: the BeBox's frames go into slirp_input(), slirp's answers
 * come back through send_packet. Slirp runs on the emulation thread,
 * polled without blocking from the network card's timer.
 */
#include "NetworkBackend.h"

#include "../Log.h"

#include <cstdlib>
#include <cstring>

namespace bebox {

bool
ParsePortForwards(const std::string& text,
	std::vector<UserNetworkConfig::Forward>& forwards, std::string& error)
{
	forwards.clear();
	size_t position = 0;
	while (position < text.size()) {
		size_t end = text.find_first_of(", ", position);
		if (end == std::string::npos)
			end = text.size();
		std::string item = text.substr(position, end - position);
		position = end + 1;
		if (item.empty())
			continue;
		UserNetworkConfig::Forward forward;
		if (item.compare(0, 4, "udp:") == 0) {
			forward.udp = true;
			item.erase(0, 4);
		} else if (item.compare(0, 4, "tcp:") == 0)
			item.erase(0, 4);
		size_t colon = item.find(':');
		char* hostEnd = nullptr;
		char* guestEnd = nullptr;
		long host = strtol(item.c_str(), &hostEnd, 10);
		long guest = colon == std::string::npos ? 0
			: strtol(item.c_str() + colon + 1, &guestEnd, 10);
		if (colon == std::string::npos || hostEnd != item.c_str() + colon
			|| guestEnd == nullptr || *guestEnd != '\0' || host < 1
			|| host > 65535 || guest < 1 || guest > 65535) {
			error = "bad port forward \"" + item
				+ "\" (expected [tcp:|udp:]HOSTPORT:GUESTPORT)";
			return false;
		}
		forward.hostPort = (u16)host;
		forward.guestPort = (u16)guest;
		forwards.push_back(forward);
	}
	return true;
}

}	// namespace bebox


#ifndef HAVE_SLIRP

namespace bebox {

std::unique_ptr<NetworkBackend>
CreateUserNetwork(const UserNetworkConfig& config, std::string& error)
{
	error = "this build has no networking (built without libslirp)";
	return nullptr;
}

}	// namespace bebox

#else	// HAVE_SLIRP

// The classic poll interface: libslirp 4.7 (Haiku) has no socket variant.
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <slirp/libslirp.h>

#include <arpa/inet.h>
#include <poll.h>

#include <chrono>
#include <deque>
#include <list>
#include <utility>

namespace bebox {

namespace {

// ssize_t up to libslirp 4.7, slirp_ssize_t later
typedef decltype(std::declval<SlirpWriteCb>()(nullptr, 0, nullptr))
	WriteResult;

class SlirpNetwork : public NetworkBackend {
public:
								SlirpNetwork();
	virtual						~SlirpNetwork();

			bool				Init(const UserNetworkConfig& config,
									std::string& error);

	virtual	void				Send(const u8* frame, size_t length);
	virtual	void				Poll();

private:
	struct Timer {
		SlirpTimerCb callback;
		void* opaque;
		int64_t expire;			// ms
		bool armed;
		bool freed;
	};

	static	WriteResult		_SendPacket(const void* buffer, size_t length,
									void* opaque);
	static	void				_GuestError(const char* message,
									void* opaque);
	static	int64_t				_ClockNs(void* opaque);
	static	void*				_TimerNew(SlirpTimerCb callback,
									void* callbackOpaque, void* opaque);
	static	void				_TimerFree(void* timer, void* opaque);
	static	void				_TimerMod(void* timer, int64_t expireTime,
									void* opaque);
	static	void				_RegisterPollFd(int fd, void* opaque) {}
	static	void				_UnregisterPollFd(int fd, void* opaque) {}
	static	void				_Notify(void* opaque) {}
	static	int					_AddPoll(int fd, int events, void* opaque);
	static	int					_GetREvents(int index, void* opaque);

			Slirp*				fSlirp;
			SlirpCb				fCallbacks;
			std::vector<pollfd>	fPollFds;
			std::deque<std::vector<u8> > fIncoming;
			std::list<Timer>	fTimers;
};


SlirpNetwork::SlirpNetwork()
	:
	fSlirp(nullptr)
{
	memset(&fCallbacks, 0, sizeof(fCallbacks));
	fCallbacks.send_packet = _SendPacket;
	fCallbacks.guest_error = _GuestError;
	fCallbacks.clock_get_ns = _ClockNs;
	fCallbacks.timer_new = _TimerNew;
	fCallbacks.timer_free = _TimerFree;
	fCallbacks.timer_mod = _TimerMod;
	fCallbacks.register_poll_fd = _RegisterPollFd;
	fCallbacks.unregister_poll_fd = _UnregisterPollFd;
	fCallbacks.notify = _Notify;
}


SlirpNetwork::~SlirpNetwork()
{
	if (fSlirp != nullptr)
		slirp_cleanup(fSlirp);
}


bool
SlirpNetwork::Init(const UserNetworkConfig& config, std::string& error)
{
	SlirpConfig slirpConfig;
	memset(&slirpConfig, 0, sizeof(slirpConfig));
	slirpConfig.version = 1;
	slirpConfig.in_enabled = true;
	inet_pton(AF_INET, "10.0.2.0", &slirpConfig.vnetwork);
	inet_pton(AF_INET, "255.255.255.0", &slirpConfig.vnetmask);
	inet_pton(AF_INET, "10.0.2.2", &slirpConfig.vhost);
	inet_pton(AF_INET, "10.0.2.15", &slirpConfig.vdhcp_start);
	inet_pton(AF_INET, "10.0.2.3", &slirpConfig.vnameserver);
	slirpConfig.vhostname = "bebox";
	fSlirp = slirp_new(&slirpConfig, &fCallbacks, this);
	if (fSlirp == nullptr) {
		error = "cannot start the user mode network";
		return false;
	}
	struct in_addr any, guest;
	any.s_addr = htonl(INADDR_ANY);
	inet_pton(AF_INET, "10.0.2.15", &guest);
	for (const UserNetworkConfig::Forward& forward : config.forwards) {
		if (slirp_add_hostfwd(fSlirp, forward.udp, any, forward.hostPort,
				guest, forward.guestPort) < 0) {
			error = "cannot forward host port "
				+ std::to_string(forward.hostPort) + " (in use?)";
			return false;
		}
	}
	return true;
}


void
SlirpNetwork::Send(const u8* frame, size_t length)
{
	slirp_input(fSlirp, frame, (int)length);
}


void
SlirpNetwork::Poll()
{
	uint32_t timeout = 0;
	fPollFds.clear();
	slirp_pollfds_fill(fSlirp, &timeout, _AddPoll, this);
	int result = fPollFds.empty() ? 0
		: poll(fPollFds.data(), fPollFds.size(), 0);
	slirp_pollfds_poll(fSlirp, result < 0, _GetREvents, this);

	int64_t now = _ClockNs(nullptr) / 1000000;
	for (Timer& timer : fTimers) {
		if (!timer.freed && timer.armed && timer.expire <= now) {
			timer.armed = false;
			timer.callback(timer.opaque);
		}
	}
	fTimers.remove_if([](const Timer& timer) { return timer.freed; });

	while (!fIncoming.empty() && fReceiver) {
		std::vector<u8>& frame = fIncoming.front();
		if (!fReceiver(frame.data(), frame.size()))
			break;
		fIncoming.pop_front();
	}
}


WriteResult
SlirpNetwork::_SendPacket(const void* buffer, size_t length, void* opaque)
{
	SlirpNetwork* network = (SlirpNetwork*)opaque;
	// a stalled guest must not make the queue grow without bounds
	if (network->fIncoming.size() < 1024) {
		const u8* bytes = (const u8*)buffer;
		network->fIncoming.emplace_back(bytes, bytes + length);
	}
	return (WriteResult)length;
}


void
SlirpNetwork::_GuestError(const char* message, void* opaque)
{
	BB_LOG(kLogHost, kLogWarning, "network: %s", message);
}


int64_t
SlirpNetwork::_ClockNs(void* opaque)
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}


void*
SlirpNetwork::_TimerNew(SlirpTimerCb callback, void* callbackOpaque,
	void* opaque)
{
	SlirpNetwork* network = (SlirpNetwork*)opaque;
	network->fTimers.push_back(Timer{ callback, callbackOpaque, 0, false,
		false });
	return &network->fTimers.back();
}


void
SlirpNetwork::_TimerFree(void* timer, void* opaque)
{
	((Timer*)timer)->freed = true;
}


void
SlirpNetwork::_TimerMod(void* timer, int64_t expireTime, void* opaque)
{
	Timer* t = (Timer*)timer;
	t->expire = expireTime;
	t->armed = true;
}


int
SlirpNetwork::_AddPoll(int fd, int events, void* opaque)
{
	SlirpNetwork* network = (SlirpNetwork*)opaque;
	pollfd entry;
	entry.fd = fd;
	entry.events = 0;
	entry.revents = 0;
	if ((events & SLIRP_POLL_IN) != 0)
		entry.events |= POLLIN;
	if ((events & SLIRP_POLL_OUT) != 0)
		entry.events |= POLLOUT;
	if ((events & SLIRP_POLL_PRI) != 0)
		entry.events |= POLLPRI;
	network->fPollFds.push_back(entry);
	return (int)network->fPollFds.size() - 1;
}


int
SlirpNetwork::_GetREvents(int index, void* opaque)
{
	SlirpNetwork* network = (SlirpNetwork*)opaque;
	if (index < 0 || (size_t)index >= network->fPollFds.size())
		return 0;
	short revents = network->fPollFds[index].revents;
	int events = 0;
	if ((revents & POLLIN) != 0)
		events |= SLIRP_POLL_IN;
	if ((revents & POLLOUT) != 0)
		events |= SLIRP_POLL_OUT;
	if ((revents & POLLPRI) != 0)
		events |= SLIRP_POLL_PRI;
	if ((revents & POLLERR) != 0)
		events |= SLIRP_POLL_ERR;
	if ((revents & POLLHUP) != 0)
		events |= SLIRP_POLL_HUP;
	return events;
}

}	// namespace


std::unique_ptr<NetworkBackend>
CreateUserNetwork(const UserNetworkConfig& config, std::string& error)
{
	std::unique_ptr<SlirpNetwork> network(new SlirpNetwork);
	if (!network->Init(config, error))
		return nullptr;
	return network;
}

}	// namespace bebox

#endif	// HAVE_SLIRP
