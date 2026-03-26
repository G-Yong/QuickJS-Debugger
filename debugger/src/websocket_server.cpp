#include "websocket_server.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
#endif

// ============================================================
// SHA-1 (Windows CryptoAPI for correctness)
// ============================================================
namespace {

void sha1_hash(const void* data, size_t len, uint8_t digest[20]) {
#ifdef _WIN32
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    memset(digest, 0, 20);
    if (!CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        fprintf(stderr, "[SHA1] CryptAcquireContext failed: %lu\n", GetLastError());
        return;
    }
    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        fprintf(stderr, "[SHA1] CryptCreateHash failed: %lu\n", GetLastError());
        CryptReleaseContext(hProv, 0);
        return;
    }
    if (!CryptHashData(hHash, (const BYTE*)data, (DWORD)len, 0)) {
        fprintf(stderr, "[SHA1] CryptHashData failed: %lu\n", GetLastError());
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return;
    }
    DWORD hashLen = 20;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, digest, &hashLen, 0)) {
        fprintf(stderr, "[SHA1] CryptGetHashParam failed: %lu\n", GetLastError());
    }
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
#else
    // Minimal SHA-1 for non-Windows platforms
    auto rol = [](uint32_t v, int b) -> uint32_t { return (v << b) | (v >> (32 - b)); };
    uint32_t h0=0x67452301, h1=0xEFCDAB89, h2=0x98BADCFE, h3=0x10325476, h4=0xC3D2E1F0;
    // Build padded message
    size_t ml = len * 8;
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(padded_len, 0);
    memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; i++) msg[padded_len - 1 - i] = (uint8_t)(ml >> (i * 8));
    // Process blocks
    for (size_t off = 0; off < padded_len; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)msg[off+i*4]<<24 | (uint32_t)msg[off+i*4+1]<<16 |
                    (uint32_t)msg[off+i*4+2]<<8 | (uint32_t)msg[off+i*4+3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
        uint32_t a=h0, b=h1, c=h2, d=h3, e=h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f=(b&c)|((~b)&d);     k=0x5A827999; }
            else if (i < 40) { f=b^c^d;               k=0x6ED9EBA1; }
            else if (i < 60) { f=(b&c)|(b&d)|(c&d);   k=0x8F1BBCDC; }
            else              { f=b^c^d;               k=0xCA62C1D6; }
            uint32_t t = rol(a,5)+f+e+k+w[i];
            e=d; d=c; c=rol(b,30); b=a; a=t;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }
    uint32_t h[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        digest[i*4]=(uint8_t)(h[i]>>24); digest[i*4+1]=(uint8_t)(h[i]>>16);
        digest[i*4+2]=(uint8_t)(h[i]>>8); digest[i*4+3]=(uint8_t)(h[i]);
    }
#endif
}

std::string base64_encode(const uint8_t* data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    r.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i+1 < len) n |= (uint32_t)data[i+1] << 8;
        if (i+2 < len) n |= data[i+2];
        r += table[(n>>18)&63];
        r += table[(n>>12)&63];
        r += (i+1 < len) ? table[(n>>6)&63] : '=';
        r += (i+2 < len) ? table[n&63] : '=';
    }
    return r;
}

std::string compute_ws_accept(const std::string& key) {
    std::string concat = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    sha1_hash(concat.data(), concat.size(), digest);
    return base64_encode(digest, 20);
}

// Parse HTTP header value by name (case-insensitive)
std::string get_header(const std::string& request, const std::string& name) {
    std::string lower_name = name;
    std::string lower_req = request;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    std::transform(lower_req.begin(), lower_req.end(), lower_req.begin(), ::tolower);
    size_t pos = lower_req.find(lower_name + ":");
    if (pos == std::string::npos) return "";
    pos += lower_name.size() + 1;
    while (pos < request.size() && request[pos] == ' ') pos++;
    size_t end = request.find("\r\n", pos);
    if (end == std::string::npos) end = request.size();
    return request.substr(pos, end - pos);
}

std::string get_request_path(const std::string& request) {
    // GET /path HTTP/1.1
    size_t start = request.find(' ');
    if (start == std::string::npos) return "/";
    start++;
    size_t end = request.find(' ', start);
    if (end == std::string::npos) return "/";
    return request.substr(start, end - start);
}

} // anonymous namespace

// ============================================================
// WebSocketServer implementation
// ============================================================

WebSocketServer::WebSocketServer() = default;

WebSocketServer::~WebSocketServer() {
    stop();
}

bool WebSocketServer::start(int port) {
    if (!platform_socket_init()) {
        fprintf(stderr, "[WS] Socket init failed\n");
        return false;
    }

    // SHA-1 / WebSocket handshake self-test (RFC 6455 example)
    {
        std::string test_input = std::string("dGhlIHNhbXBsZSBub25jZQ==") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        uint8_t test_digest[20];
        sha1_hash(test_input.data(), test_input.size(), test_digest);
        fprintf(stderr, "[WS] SHA-1 hex: ");
        for (int i = 0; i < 20; i++) fprintf(stderr, "%02x", test_digest[i]);
        fprintf(stderr, "\n");
        std::string test = base64_encode(test_digest, 20);
        const char* expected = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
        fprintf(stderr, "[WS] SHA-1 self-test: got='%s' expected='%s' %s\n",
                test.c_str(), expected,
                (test == expected) ? "OK" : "MISMATCH!");
    }

    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCK) {
        fprintf(stderr, "[WS] Cannot create socket\n");
        return false;
    }

    // Allow port reuse
    int opt = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_sock_, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[WS] Bind failed on port %d (error %d)\n", port, platform_socket_error());
        close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
        return false;
    }

    if (listen(listen_sock_, 1) != 0) {
        fprintf(stderr, "[WS] Listen failed\n");
        close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
        return false;
    }

    port_ = port;
    return true;
}

void WebSocketServer::stop() {
    disconnect();
    if (listen_sock_ != INVALID_SOCK) {
        close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
    }
    platform_socket_cleanup();
}

void WebSocketServer::disconnect() {
    connected_.store(false);
    if (client_sock_ != INVALID_SOCK) {
        close_socket(client_sock_);
        client_sock_ = INVALID_SOCK;
    }
}

std::string WebSocketServer::read_http_request(socket_t sock) {
    std::string request;
    char buf[1];
    while (request.size() < 8192) {
        int n = recv(sock, buf, 1, 0);
        if (n <= 0) return "";
        request += buf[0];
        if (request.size() >= 4 &&
            request.substr(request.size()-4) == "\r\n\r\n") {
            return request;
        }
    }
    return "";
}

bool WebSocketServer::wait_for_connection(const std::string& target_name,
                                           const std::string& target_url) {
    target_name_ = target_name;
    target_url_ = target_url;

    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        socket_t sock = accept(listen_sock_, (struct sockaddr*)&client_addr, &addr_len);
        if (sock == INVALID_SOCK) {
            fprintf(stderr, "[WS] Accept failed\n");
            return false;
        }

        // Disable Nagle for low latency
        int flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));

        std::string request = read_http_request(sock);
        if (request.empty()) {
            close_socket(sock);
            continue;
        }

        std::string path = get_request_path(request);
        std::string upgrade = get_header(request, "Upgrade");

        // Check for WebSocket upgrade
        std::string upgrade_lower = upgrade;
        std::transform(upgrade_lower.begin(), upgrade_lower.end(), upgrade_lower.begin(), ::tolower);

        if (upgrade_lower == "websocket") {
            // Log the upgrade request for debugging
            fprintf(stderr, "[WS] Upgrade request on path '%s':\n", path.c_str());
            // Print key headers
            fprintf(stderr, "[WS]   Sec-WebSocket-Key: %s\n", get_header(request, "Sec-WebSocket-Key").c_str());
            fprintf(stderr, "[WS]   Sec-WebSocket-Version: %s\n", get_header(request, "Sec-WebSocket-Version").c_str());
            fprintf(stderr, "[WS]   Sec-WebSocket-Protocol: %s\n", get_header(request, "Sec-WebSocket-Protocol").c_str());

            client_sock_ = sock;
            if (!do_websocket_handshake(request)) {
                fprintf(stderr, "[WS] Handshake failed, waiting for new connection\n");
                close_socket(sock);
                client_sock_ = INVALID_SOCK;
                continue;
            }
            connected_.store(true);
            fprintf(stderr, "[WS] DevTools connected (path=%s)\n", path.c_str());
            return true;
        }

        // Handle HTTP discovery endpoints
        if (path == "/json/version") {
            std::string body = "{\"Browser\":\"QuickJS-Debug/1.0\",\"Protocol-Version\":\"1.3\"}";
            send_http_response(sock, 200, "OK", "application/json", body);
            close_socket(sock);
            continue;
        }

        if (path == "/json" || path == "/json/list") {
            char body[2048];
            snprintf(body, sizeof(body),
                "[{"
                "\"description\":\"QuickJS instance\","
                "\"devtoolsFrontendUrl\":\"devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=127.0.0.1:%d/debug\","
                "\"id\":\"main\","
                "\"title\":\"%s\","
                "\"type\":\"node\","
                "\"url\":\"%s\","
                "\"webSocketDebuggerUrl\":\"ws://127.0.0.1:%d/debug\""
                "}]",
                port_, target_name_.c_str(), target_url_.c_str(), port_);
            send_http_response(sock, 200, "OK", "application/json", body);
            close_socket(sock);
            continue;
        }

        // Unknown path
        send_http_response(sock, 404, "Not Found", "text/plain", "Not Found");
        close_socket(sock);
    }
}

bool WebSocketServer::send_http_response(socket_t sock, int code, const std::string& status,
                                          const std::string& content_type, const std::string& body) {
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s; charset=UTF-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, status.c_str(), content_type.c_str(), body.size());

    std::string response = std::string(header) + body;
    int sent = ::send(sock, response.c_str(), (int)response.size(), 0);
    return sent == (int)response.size();
}

bool WebSocketServer::do_websocket_handshake(const std::string& request) {
    std::string key = get_header(request, "Sec-WebSocket-Key");
    if (key.empty()) {
        fprintf(stderr, "[WS] Handshake failed: no Sec-WebSocket-Key\n");
        return false;
    }

    std::string accept = compute_ws_accept(key);
    std::string protocol = get_header(request, "Sec-WebSocket-Protocol");

    // Log handshake details
    fprintf(stderr, "[WS] Handshake: key='%s' protocol='%s'\n",
            key.c_str(), protocol.c_str());

    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + accept + "\r\n";
    if (!protocol.empty()) {
        response += "Sec-WebSocket-Protocol: " + protocol + "\r\n";
    }
    response += "\r\n";

    fprintf(stderr, "[WS] Sending handshake response (%d bytes):\n%s", (int)response.size(), response.c_str());

    int len = (int)response.size();
    return ::send(client_sock_, response.c_str(), len, 0) == len;
}

bool WebSocketServer::send(const std::string& message) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!connected_.load() || client_sock_ == INVALID_SOCK) return false;
    return send_ws_frame(0x01, (const uint8_t*)message.data(), message.size());
}

std::string WebSocketServer::receive() {
    if (!connected_.load() || client_sock_ == INVALID_SOCK) {
        fprintf(stderr, "[WS] receive(): not connected (connected=%d sock=%d)\n",
                (int)connected_.load(), (int)(client_sock_ != INVALID_SOCK));
        return "";
    }

    std::string payload;
    uint8_t opcode;
    if (!read_ws_frame(payload, opcode)) {
        fprintf(stderr, "[WS] receive(): read_ws_frame failed\n");
        connected_.store(false);
        return "";
    }

    if (opcode == 0x08) {
        // Close frame
        connected_.store(false);
        // Send close frame back
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_ws_frame(0x08, nullptr, 0);
        return "";
    }

    if (opcode == 0x09) {
        // Ping -> Pong
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_ws_frame(0x0A, (const uint8_t*)payload.data(), payload.size());
        return receive(); // Read next frame
    }

    if (opcode == 0x0A) {
        // Pong - ignore
        return receive();
    }

    return payload;
}

bool WebSocketServer::send_ws_frame(uint8_t opcode, const uint8_t* data, size_t len) {
    uint8_t header[10];
    size_t header_len;

    header[0] = 0x80 | (opcode & 0x0F); // FIN + opcode

    if (len < 126) {
        header[1] = (uint8_t)len;
        header_len = 2;
    } else if (len < 65536) {
        header[1] = 126;
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)(len & 0xFF);
        header_len = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++)
            header[2+i] = (uint8_t)(len >> ((7-i)*8));
        header_len = 10;
    }

    if (::send(client_sock_, (const char*)header, (int)header_len, 0) != (int)header_len)
        return false;

    if (len > 0 && data) {
        size_t sent = 0;
        while (sent < len) {
            int n = ::send(client_sock_, (const char*)data + sent, (int)(len - sent), 0);
            if (n <= 0) return false;
            sent += n;
        }
    }
    return true;
}

bool WebSocketServer::recv_exact(void* buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        int n = recv(client_sock_, (char*)buf + received, (int)(len - received), 0);
        if (n <= 0) {
            fprintf(stderr, "[WS] recv_exact: recv returned %d (wanted %zu more), error=%d\n",
                    n, len - received, platform_socket_error());
            return false;
        }
        received += n;
    }
    return true;
}

bool WebSocketServer::read_ws_frame(std::string& payload, uint8_t& opcode) {
    uint8_t header[2];
    if (!recv_exact(header, 2)) return false;

    opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        if (!recv_exact(ext, 2)) return false;
        payload_len = (uint64_t)ext[0] << 8 | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (!recv_exact(ext, 8)) return false;
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | ext[i];
    }

    uint8_t mask[4] = {};
    if (masked) {
        if (!recv_exact(mask, 4)) return false;
    }

    payload.resize((size_t)payload_len);
    if (payload_len > 0) {
        if (!recv_exact(&payload[0], (size_t)payload_len)) return false;
        if (masked) {
            for (size_t i = 0; i < payload_len; i++)
                payload[i] ^= mask[i % 4];
        }
    }

    return true;
}
