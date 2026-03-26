#pragma once

#include "platform.h"
#include <string>
#include <mutex>
#include <atomic>

class WebSocketServer {
public:
    WebSocketServer();
    ~WebSocketServer();

    bool start(int port);
    void stop();

    // Accept loop: handles HTTP discovery requests, then upgrades to WebSocket.
    // Blocks until a WebSocket client connects.
    bool wait_for_connection(const std::string& target_name, const std::string& target_url);

    // Thread-safe send (can be called from any thread)
    bool send(const std::string& message);

    // Blocking receive. Returns empty string on error/close.
    std::string receive();

    bool is_connected() const { return connected_.load(); }
    int port() const { return port_; }

    void disconnect();

private:
    bool send_http_response(socket_t sock, int code, const std::string& status,
                            const std::string& content_type, const std::string& body);
    bool do_websocket_handshake(const std::string& request);
    bool send_ws_frame(uint8_t opcode, const uint8_t* data, size_t len);
    bool read_ws_frame(std::string& payload, uint8_t& opcode);
    bool recv_exact(void* buf, size_t len);
    std::string read_http_request(socket_t sock);

    socket_t listen_sock_ = INVALID_SOCK;
    socket_t client_sock_ = INVALID_SOCK;
    int port_ = 0;
    std::atomic<bool> connected_{false};
    std::mutex send_mutex_;
    std::string target_name_;
    std::string target_url_;
};
