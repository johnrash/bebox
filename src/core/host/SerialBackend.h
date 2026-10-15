/*
 * BeBox emulator - host side of an emulated serial port.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Transmit() and the modem control calls come from the emulation thread.
 * Receive() is polled by the emulation thread; implementations that get
 * data from other threads must make it thread safe.
 */
#ifndef BEBOX_SERIAL_BACKEND_H
#define BEBOX_SERIAL_BACKEND_H

#include "../Types.h"

namespace bebox {

struct SerialLineConfig {
	u32 baud = 9600;
	int dataBits = 8;
	char parity = 'N';
	int stopBits = 1;
};

struct SerialModemStatus {
	bool cts = true;
	bool dsr = true;
	bool dcd = true;
	bool ri = false;
};

class SerialBackend {
public:
	virtual ~SerialBackend() {}

	virtual void Transmit(u8 byte) = 0;
	// Returns the next received byte or -1.
	virtual int Receive() { return -1; }
	virtual void SetLineConfig(const SerialLineConfig&) {}
	virtual void SetModemControl(bool dtr, bool rts) {}
	virtual SerialModemStatus ModemStatus() { return SerialModemStatus(); }
	// A break condition on the transmit line.
	virtual void SetBreak(bool active) {}
};

}	// namespace bebox

#endif	// BEBOX_SERIAL_BACKEND_H
