#pragma once

// Cross-platform socket abstractions

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>

    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCK = INVALID_SOCKET;

    inline void close_socket(socket_t s) { closesocket(s); }
    inline bool platform_socket_init() {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }
    inline void platform_socket_cleanup() { WSACleanup(); }
    inline int platform_socket_error() { return WSAGetLastError(); }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <signal.h>

    using socket_t = int;
    constexpr socket_t INVALID_SOCK = -1;

    inline void close_socket(socket_t s) { close(s); }
    inline bool platform_socket_init() {
        // Ignore SIGPIPE
        signal(SIGPIPE, SIG_IGN);
        return true;
    }
    inline void platform_socket_cleanup() {}
    inline int platform_socket_error() { return errno; }
#endif

#include <cstdint>
#include <cstring>
