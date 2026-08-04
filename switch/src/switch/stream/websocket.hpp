#pragma once

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gnx::stream {

// Small RFC 6455 client for the Boosteroid control socket. libcurl in the
// Switch port predates its WebSocket API, so this keeps the dependency surface
// small and uses the same mbedTLS bundle as the HTTPS client.
class WssClient {
public:
    WssClient();
    ~WssClient();

    WssClient(const WssClient&) = delete;
    WssClient& operator=(const WssClient&) = delete;

    bool connect(const std::string& hostAndPort, const std::string& path,
                 const std::string& caBundle, std::string& error);
    void close();

    bool connected() const { return connected_; }
    bool send_text(const std::string& text);
    bool send_ping();
    int last_ping_ms() const { return lastPingMs_.load(); }

    // Returns true only when one complete text message was read. A timeout is
    // normal and returns false with error empty; protocol/network failures put
    // a human-readable value in error and close the socket.
    bool read_text(std::string& text, int timeoutMs, std::string& error);

private:
    bool send_frame_locked(uint8_t opcode, const void* data, std::size_t size);
    bool write_all_locked(const unsigned char* data, std::size_t size);
    bool read_exact_locked(unsigned char* data, std::size_t size, int timeoutMs,
                           bool& timedOut, std::string& error);
    void reset_locked();

    mutable std::mutex ioMutex_;
    int socket_{-1};
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config config_;
    mbedtls_x509_crt ca_;
    mbedtls_ctr_drbg_context random_;
    mbedtls_entropy_context entropy_;
    bool initialized_{false};
    bool connected_{false};
    std::atomic<int> lastPingMs_{-1};
    std::vector<unsigned char> fragment_;
    uint8_t fragmentOpcode_{0};
};

}  // namespace gnx::stream
