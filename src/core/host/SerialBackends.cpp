/*
 * BeBox emulator - portable serial port backends (POSIX).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "SerialBackends.h"

#include "../Log.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#	define MSG_NOSIGNAL 0
#endif

namespace bebox {

namespace {

void
SetNonBlocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


bool
ResolveAddress(const std::string& host, int port, sockaddr_in& address,
	std::string& error)
{
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((uint16_t)port);
	if (host.empty() || host == "localhost") {
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		return true;
	}
	if (host == "*" || host == "any") {
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		return true;
	}
	if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1)
		return true;
	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* result = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0
		|| result == nullptr) {
		error = "unknown host " + host;
		return false;
	}
	address.sin_addr = ((sockaddr_in*)result->ai_addr)->sin_addr;
	freeaddrinfo(result);
	return true;
}

}	// namespace


// #pragma mark - FileSerialLog


FileSerialLog::~FileSerialLog()
{
	if (fFd >= 0)
		close(fFd);
}


void
FileSerialLog::Transmit(u8 byte)
{
	if (fFd >= 0 && write(fFd, &byte, 1) != 1) {
		close(fFd);
		fFd = -1;
	}
}


// #pragma mark - TcpSerial


TcpSerial::~TcpSerial()
{
	_Disconnect();
	if (fListener >= 0)
		close(fListener);
}


std::unique_ptr<TcpSerial>
TcpSerial::Listen(const std::string& host, int port, std::string& error)
{
	sockaddr_in address;
	if (!ResolveAddress(host, port, address, error))
		return nullptr;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		error = std::string("socket: ") + strerror(errno);
		return nullptr;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(fd, (sockaddr*)&address, sizeof(address)) != 0
		|| listen(fd, 1) != 0) {
		error = "TCP port " + std::to_string(port) + ": " + strerror(errno);
		close(fd);
		return nullptr;
	}
	SetNonBlocking(fd);
	std::unique_ptr<TcpSerial> serial(new TcpSerial);
	serial->fListener = fd;
	serial->fPort = port;
	return serial;
}


std::unique_ptr<TcpSerial>
TcpSerial::Connect(const std::string& host, int port, std::string& error)
{
	sockaddr_in address;
	if (!ResolveAddress(host, port, address, error))
		return nullptr;
	std::unique_ptr<TcpSerial> serial(new TcpSerial);
	serial->fConnectMode = true;
	serial->fHost = host;
	serial->fPort = port;
	serial->_Service();
	return serial;
}


void
TcpSerial::_Disconnect()
{
	if (fClient >= 0) {
		close(fClient);
		fClient = -1;
	}
	fInLength = fInPosition = 0;
	fOut.clear();
}


// Accepts or (re)connects, and moves data.
void
TcpSerial::_Service()
{
	if (fClient < 0) {
		if (fRetryCountdown > 0) {
			fRetryCountdown--;
			return;
		}
		if (fListener >= 0) {
			int client = accept(fListener, nullptr, nullptr);
			if (client < 0)
				return;
			fClient = client;
		} else if (fConnectMode) {
			sockaddr_in address;
			std::string error;
			fRetryCountdown = 500;		// about a second of polls
			if (!ResolveAddress(fHost, fPort, address, error))
				return;
			int fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd < 0)
				return;
			if (connect(fd, (sockaddr*)&address, sizeof(address)) != 0) {
				close(fd);
				return;
			}
			fClient = fd;
		} else
			return;
		SetNonBlocking(fClient);
		int one = 1;
		setsockopt(fClient, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		BB_LOG(kLogSerial, kLogInfo, "serial: TCP client connected (port %d)",
			fPort);
	}
	_Flush();
	if (fInPosition >= fInLength && fClient >= 0) {
		ssize_t count = recv(fClient, fIn, sizeof(fIn), 0);
		if (count > 0) {
			fInLength = (int)count;
			fInPosition = 0;
		} else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK
				&& errno != EINTR)) {
			BB_LOG(kLogSerial, kLogInfo, "serial: TCP client disconnected");
			_Disconnect();
		}
	}
}


void
TcpSerial::_Flush()
{
	while (!fOut.empty() && fClient >= 0) {
		u8 buffer[512];
		size_t length = 0;
		while (length < sizeof(buffer) && length < fOut.size()) {
			buffer[length] = fOut[length];
			length++;
		}
		ssize_t sent = send(fClient, buffer, length, MSG_NOSIGNAL);
		if (sent <= 0) {
			if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK
				&& errno != EINTR)
				_Disconnect();
			return;
		}
		fOut.erase(fOut.begin(), fOut.begin() + sent);
	}
}


void
TcpSerial::Transmit(u8 byte)
{
	if (fClient < 0)
		return;		// nobody listening: the byte is lost, like a cable
	if (fOut.size() < 65536)
		fOut.push_back(byte);
	_Flush();
}


int
TcpSerial::Receive()
{
	_Service();
	if (fInPosition < fInLength)
		return fIn[fInPosition++];
	return -1;
}


SerialModemStatus
TcpSerial::ModemStatus()
{
	SerialModemStatus status;
	status.dcd = status.dsr = status.cts = fClient >= 0;
	return status;
}


// #pragma mark - HostSerial


HostSerial::~HostSerial()
{
	if (fFd >= 0)
		close(fFd);
}


std::unique_ptr<HostSerial>
HostSerial::Open(const std::string& device, std::string& error)
{
	int fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		error = device + ": " + strerror(errno);
		return nullptr;
	}
	termios settings;
	if (tcgetattr(fd, &settings) == 0) {
		cfmakeraw(&settings);
		settings.c_cflag |= CLOCAL | CREAD;
		tcsetattr(fd, TCSANOW, &settings);
	}
	std::unique_ptr<HostSerial> serial(new HostSerial);
	serial->fFd = fd;
	return serial;
}


void
HostSerial::Transmit(u8 byte)
{
	if (fFd >= 0)
		(void)write(fFd, &byte, 1);
}


int
HostSerial::Receive()
{
	if (fInPosition >= fInLength) {
		ssize_t count = read(fFd, fIn, sizeof(fIn));
		if (count <= 0)
			return -1;
		fInLength = (int)count;
		fInPosition = 0;
	}
	return fIn[fInPosition++];
}


void
HostSerial::SetLineConfig(const SerialLineConfig& config)
{
	termios settings;
	if (tcgetattr(fFd, &settings) != 0)
		return;
	static const struct { u32 baud; speed_t speed; } kSpeeds[] = {
		{ 50, B50 }, { 75, B75 }, { 110, B110 }, { 134, B134 },
		{ 150, B150 }, { 200, B200 }, { 300, B300 }, { 600, B600 },
		{ 1200, B1200 }, { 1800, B1800 }, { 2400, B2400 },
		{ 4800, B4800 }, { 9600, B9600 }, { 19200, B19200 },
		{ 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 },
		{ 230400, B230400 }
	};
	speed_t speed = B9600;
	for (const auto& entry : kSpeeds) {
		if (entry.baud <= config.baud)
			speed = entry.speed;
	}
	cfsetispeed(&settings, speed);
	cfsetospeed(&settings, speed);
	settings.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
	switch (config.dataBits) {
		case 5: settings.c_cflag |= CS5; break;
		case 6: settings.c_cflag |= CS6; break;
		case 7: settings.c_cflag |= CS7; break;
		default: settings.c_cflag |= CS8; break;
	}
	if (config.parity == 'E')
		settings.c_cflag |= PARENB;
	else if (config.parity == 'O')
		settings.c_cflag |= PARENB | PARODD;
	if (config.stopBits == 2)
		settings.c_cflag |= CSTOPB;
	tcsetattr(fFd, TCSANOW, &settings);
}


void
HostSerial::SetModemControl(bool dtr, bool rts)
{
#ifdef TIOCMGET
	int lines = 0;
	if (ioctl(fFd, TIOCMGET, &lines) != 0)
		return;
	lines = (lines & ~(TIOCM_DTR | TIOCM_RTS)) | (dtr ? TIOCM_DTR : 0)
		| (rts ? TIOCM_RTS : 0);
	ioctl(fFd, TIOCMSET, &lines);
#endif
}


SerialModemStatus
HostSerial::ModemStatus()
{
	SerialModemStatus status;
#ifdef TIOCMGET
	int lines = 0;
	if (ioctl(fFd, TIOCMGET, &lines) == 0) {
		status.cts = (lines & TIOCM_CTS) != 0;
		status.dsr = (lines & TIOCM_DSR) != 0;
		status.dcd = (lines & TIOCM_CAR) != 0;
		status.ri = (lines & TIOCM_RNG) != 0;
	}
#endif
	return status;
}


void
HostSerial::SetBreak(bool active)
{
#ifdef TIOCSBRK
	ioctl(fFd, active ? TIOCSBRK : TIOCCBRK, 0);
#endif
}


// #pragma mark -


std::unique_ptr<SerialBackend>
CreateSerialBackend(const std::string& destination, std::string& error)
{
	size_t colon = destination.find(':');
	std::string kind = destination.substr(0, colon);
	std::string rest = colon == std::string::npos ? ""
		: destination.substr(colon + 1);
	if (kind == "file") {
		int fd = open(rest.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd < 0) {
			error = rest + ": " + strerror(errno);
			return nullptr;
		}
		return std::unique_ptr<SerialBackend>(new FileSerialLog(fd));
	}
	if (kind == "tcp-listen" || kind == "tcp-connect") {
		size_t last = rest.rfind(':');
		std::string host = last == std::string::npos ? ""
			: rest.substr(0, last);
		int port = atoi(last == std::string::npos ? rest.c_str()
			: rest.c_str() + last + 1);
		if (port <= 0 || port > 65535) {
			error = "bad TCP port in " + destination;
			return nullptr;
		}
		if (kind == "tcp-listen")
			return TcpSerial::Listen(host, port, error);
		return TcpSerial::Connect(host, port, error);
	}
	if (kind == "host")
		return HostSerial::Open(rest, error);
	error = "unknown serial destination " + destination;
	return nullptr;
}

}	// namespace bebox
