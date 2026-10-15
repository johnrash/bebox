/*
 * BeBox emulator - host side of the emulated Ethernet.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Carries Ethernet frames between the emulated network card and the host.
 * Everything runs on the emulation thread: the card calls Poll()
 * regularly, and the backend hands received frames to the receiver,
 * keeping them queued while the receiver (the card's ring buffer) is full.
 */
#ifndef BEBOX_NETWORK_BACKEND_H
#define BEBOX_NETWORK_BACKEND_H

#include "../Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bebox {

class NetworkBackend {
public:
	// Takes a frame received for the BeBox; false keeps it queued.
	typedef std::function<bool(const u8* frame, size_t length)> Receiver;

	virtual						~NetworkBackend() {}

			void				SetReceiver(Receiver receiver)
									{ fReceiver = receiver; }

	// A frame sent by the BeBox.
	virtual	void				Send(const u8* frame, size_t length) = 0;
	// Services the host side and delivers pending frames.
	virtual	void				Poll() = 0;

protected:
			Receiver			fReceiver;
};


// User mode networking (NAT, like QEMU's "user" network): the BeBox is
// 10.0.2.15 (DHCP), the gateway 10.0.2.2 is the host, 10.0.2.3 the DNS
// relay.
struct UserNetworkConfig {
	struct Forward {
		bool udp = false;
		u16 hostPort = 0;
		u16 guestPort = 0;
	};
	std::vector<Forward> forwards;		// host ports to the BeBox
};

// Parses "tcp:2121:21,udp:5000:5000" (protocol optional, TCP).
bool ParsePortForwards(const std::string& text,
	std::vector<UserNetworkConfig::Forward>& forwards, std::string& error);

// nullptr (with the reason) if the emulator was built without libslirp.
std::unique_ptr<NetworkBackend> CreateUserNetwork(
	const UserNetworkConfig& config, std::string& error);

}	// namespace bebox

#endif	// BEBOX_NETWORK_BACKEND_H
