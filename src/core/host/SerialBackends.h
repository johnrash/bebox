/*
 * BeBox emulator - portable serial port backends (POSIX).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * All work happens on the emulation thread with non-blocking calls: the
 * UART polls Receive() about once per character time.
 *
 *   FileSerialLog   output to a file (no input)
 *   TcpSerial       a TCP server ("listen", one client at a time) or
 *                   client ("connect", reconnecting); raw bytes
 *   HostSerial      a host serial device (/dev/ports/... on Haiku,
 *                   /dev/tty... on Linux) with the guest's line settings
 *
 * CreateSerialBackend() parses a destination: "file:PATH",
 * "tcp-listen:[ADDRESS:]PORT", "tcp-connect:HOST:PORT", "host:DEVICE".
 */
#ifndef BEBOX_SERIAL_BACKENDS_H
#define BEBOX_SERIAL_BACKENDS_H

#include "SerialBackend.h"

#include <deque>
#include <memory>
#include <string>

namespace bebox {

class FileSerialLog : public SerialBackend {
public:
								FileSerialLog(int fd) : fFd(fd) {}
	virtual						~FileSerialLog();

	virtual	void				Transmit(u8 byte);

private:
			int					fFd;
};


class TcpSerial : public SerialBackend {
public:
	virtual						~TcpSerial();

	static	std::unique_ptr<TcpSerial> Listen(const std::string& address,
									int port, std::string& error);
	static	std::unique_ptr<TcpSerial> Connect(const std::string& host,
									int port, std::string& error);

	virtual	void				Transmit(u8 byte);
	virtual	int					Receive();
	virtual	SerialModemStatus	ModemStatus();

			bool				IsConnected() const { return fClient >= 0; }

private:
								TcpSerial() {}
			void				_Service();
			void				_Disconnect();
			void				_Flush();

			int					fListener = -1;
			int					fClient = -1;
			bool				fConnectMode = false;
			std::string			fHost;
			int					fPort = 0;
			int					fRetryCountdown = 0;
			u8					fIn[256];
			int					fInLength = 0;
			int					fInPosition = 0;
			std::deque<u8>		fOut;
};


class HostSerial : public SerialBackend {
public:
	virtual						~HostSerial();

	static	std::unique_ptr<HostSerial> Open(const std::string& device,
									std::string& error);

	virtual	void				Transmit(u8 byte);
	virtual	int					Receive();
	virtual	void				SetLineConfig(const SerialLineConfig& config);
	virtual	void				SetModemControl(bool dtr, bool rts);
	virtual	SerialModemStatus	ModemStatus();
	virtual	void				SetBreak(bool active);

private:
								HostSerial() {}

			int					fFd = -1;
			u8					fIn[256];
			int					fInLength = 0;
			int					fInPosition = 0;
};


std::unique_ptr<SerialBackend> CreateSerialBackend(
	const std::string& destination, std::string& error);

}	// namespace bebox

#endif	// BEBOX_SERIAL_BACKENDS_H
