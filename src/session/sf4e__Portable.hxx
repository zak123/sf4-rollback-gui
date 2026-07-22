#pragma once

// Portability shims for the session layer, which runs on Windows (the
// game, the desktop app) and on Linux (the dedicated Lobbyd server):
// raw UDP sockets for the NAT probe/relay machinery, a monotonic
// millisecond clock, and a sleep. GameNetworkingSockets abstracts its
// own I/O; these cover the code that talks to sockets directly.

#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

// sscanf_s and sscanf share a signature for numeric-only formats; %s
// conversions differ (sscanf_s takes a size argument) and must be
// guarded at the call site instead.
#define SF4E_SSCANF sscanf_s

namespace sf4e {
	namespace Portable {
		typedef SOCKET Socket;
		typedef int SockLen;
		const Socket kInvalidSocket = INVALID_SOCKET;

		inline void CloseSocket(Socket s) {
			closesocket(s);
		}

		inline bool SetNonBlocking(Socket s) {
			u_long nonblock = 1;
			return ioctlsocket(s, FIONBIO, &nonblock) == 0;
		}

		inline void SetRecvTimeoutMs(Socket s, unsigned int ms) {
			DWORD t = ms;
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
		}
	}
}

#else

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define SF4E_SSCANF sscanf

namespace sf4e {
	namespace Portable {
		typedef int Socket;
		typedef socklen_t SockLen;
		const Socket kInvalidSocket = -1;

		inline void CloseSocket(Socket s) {
			close(s);
		}

		inline bool SetNonBlocking(Socket s) {
			int flags = fcntl(s, F_GETFL, 0);
			return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
		}

		inline void SetRecvTimeoutMs(Socket s, unsigned int ms) {
			struct timeval tv;
			tv.tv_sec = ms / 1000;
			tv.tv_usec = (ms % 1000) * 1000;
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
		}
	}
}

#endif

#include <chrono>
#include <thread>

namespace sf4e {
	namespace Portable {
		// Monotonic milliseconds, GetTickCount64-shaped: only deltas
		// between values from the same process are meaningful.
		inline uint64_t NowMs() {
			return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()
			).count();
		}

		inline void SleepMs(unsigned int ms) {
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		}

		// Byte n (0 = leftmost in dotted notation) of a network-order
		// IPv4 address, for logging; in_addr's byte-access union is
		// Windows-only.
		inline unsigned int IpByte(uint32_t addrNetOrder, int n) {
			return (ntohl(addrNetOrder) >> (8 * (3 - n))) & 0xff;
		}
	}
}
