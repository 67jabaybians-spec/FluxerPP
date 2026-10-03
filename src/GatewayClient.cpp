// src/GatewayClient.cpp
//
// Cross-platform (Windows / Linux / macOS) gateway client.
// Transport is libcurl's WebSocket API (curl >= 7.86, stable from 8.11), the
// same library RestClient already uses, so there is no new dependency.
#include "fluxerpp/GatewayClient.h"
#include "fluxerpp/RestClient.h"

#include "fluxerpp/models/Guild.h"
#include "fluxerpp/models/Message.h"
#include "fluxerpp/util/Logger.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
  #include <winsock2.h>   // WSAPoll
#else
  #include <poll.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if LIBCURL_VERSION_NUM < 0x075600
  #error "fluxerpp GatewayClient needs libcurl >= 7.86.0 with WebSocket support (8.11+ recommended)"
#endif

namespace fluxerpp {

using util::Logger;

// ---------------------------------------------------------------------------
// URL helper
// ---------------------------------------------------------------------------

// Splits a "wss://host[:port]/path?query" URL into scheme, host and path+query.
// Path defaults to "/" and scheme defaults to "wss" if absent.
struct ParsedWsUrl {
    std::string host;
    std::string path;
    std::string scheme = "wss";
};

static ParsedWsUrl parse_ws_url(const std::string& url) {
    ParsedWsUrl out;
    std::string rest = url;
    auto schemePos = rest.find("://");
    if (schemePos != std::string::npos) {
        out.scheme = rest.substr(0, schemePos);
        rest = rest.substr(schemePos + 3);
    }

    auto slashPos = rest.find('/');
    if (slashPos == std::string::npos) {
        out.host = rest;
        out.path = "/";
    } else {
        out.host = rest.substr(0, slashPos);
        out.path = rest.substr(slashPos);
        if (out.path.empty()) out.path = "/";
    }
    return out;
}

// ---------------------------------------------------------------------------
// libcurl global init (once, thread-safe)
// ---------------------------------------------------------------------------

static void ensure_curl_init() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// ---------------------------------------------------------------------------
// WsConnection: owns one libcurl WebSocket connection (RAII)
// ---------------------------------------------------------------------------
//
// libcurl easy handles are NOT thread-safe, but this client sends from the
// heartbeat thread while the main thread receives. All curl_ws_* calls
// therefore go through `mu_`. That is cheap because the receive side is
// non-blocking (CURLE_AGAIN): the main thread holds the lock only for the
// instant it takes to drain what is already buffered, and does its waiting
// in wait_readable() *outside* the lock.
//
// Cancelling a blocked receive (stop(), missed heartbeat ACK) no longer needs
// the "close the handle from another thread" trick WinHTTP required. Callers
// set an atomic flag and the receive loop notices within kPollMs.
class WsConnection {
public:
    enum class RecvStatus { Message, Again, Close, Error };

    WsConnection() = default;
    WsConnection(const WsConnection&) = delete;
    WsConnection& operator=(const WsConnection&) = delete;
    ~WsConnection() {
        if (curl_) curl_easy_cleanup(curl_);
    }

    bool open(const std::string& url, std::string& err) {
        curl_ = curl_easy_init();
        if (!curl_) { err = "curl_easy_init failed"; return false; }

        errbuf_[0] = '\0';
        curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, errbuf_);
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CONNECT_ONLY, 2L); // 2 = WebSocket
        curl_easy_setopt(curl_, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl_, CURLOPT_USERAGENT, "FluxerPP/1.0");
        // Deliberately no CURLOPT_TIMEOUT: it would kill a healthy long-lived socket.

        CURLcode rc = curl_easy_perform(curl_); // performs TCP + TLS + WS upgrade
        if (rc != CURLE_OK) {
            err = std::string(curl_easy_strerror(rc));
            if (errbuf_[0]) err += std::string(" (") + errbuf_ + ")";
            return false;
        }

        if (curl_easy_getinfo(curl_, CURLINFO_ACTIVESOCKET, &sock_) != CURLE_OK) {
            sock_ = CURL_SOCKET_BAD;
        }
        return true;
    }

    // Sends one complete text message. Thread-safe.
    CURLcode send_text(const std::string& payload) {
        std::lock_guard<std::mutex> lk(mu_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        size_t off = 0;
        while (off < payload.size()) {
            size_t sent = 0;
            CURLcode rc = curl_ws_send(curl_, payload.data() + off, payload.size() - off,
                                       &sent, 0, CURLWS_TEXT);
            off += sent;
            if (rc == CURLE_AGAIN) {
                if (std::chrono::steady_clock::now() > deadline) return CURLE_OPERATION_TIMEDOUT;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (rc != CURLE_OK) return rc;
        }
        return CURLE_OK;
    }

    // Best-effort polite close. Thread-safe.
    void send_close(std::uint16_t code) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!curl_) return;
        unsigned char payload[2] = {
            static_cast<unsigned char>(code >> 8),
            static_cast<unsigned char>(code & 0xFF)
        };
        size_t sent = 0;
        curl_ws_send(curl_, payload, sizeof(payload), &sent, 0, CURLWS_CLOSE);
    }

    // Non-blocking. Appends text-frame payload bytes to `acc`.
    //   Message : `acc` now holds one complete, reassembled text message
    //   Again   : nothing (more) to read right now; `acc` may hold a partial message
    //   Close   : server sent a CLOSE frame; `closeCode` is set (0 if none given)
    //   Error   : transport failed; see last_error()
    //
    // Fragment reassembly: libcurl reports each frame/fragment with flags. A
    // message is complete once a chunk has no CURLWS_CONT flag AND no bytes
    // remain in the current frame. Same idea as the old WinHTTP
    // FRAGMENT / MESSAGE buffer types.
    RecvStatus recv_message(std::string& acc, int& closeCode) {
        std::lock_guard<std::mutex> lk(mu_);
        char buf[16 * 1024];

        for (;;) {
            size_t got = 0;
            const struct curl_ws_frame* meta = nullptr;
            CURLcode rc = curl_ws_recv(curl_, buf, sizeof(buf), &got, &meta);

            if (rc == CURLE_AGAIN) return RecvStatus::Again;
            if (rc != CURLE_OK) {
                last_error_ = curl_easy_strerror(rc);
                if (errbuf_[0]) last_error_ += std::string(" (") + errbuf_ + ")";
                return RecvStatus::Error;
            }
            if (!meta) continue;

            if (meta->flags & CURLWS_CLOSE) {
                closeCode = (got >= 2)
                    ? ((static_cast<unsigned char>(buf[0]) << 8) | static_cast<unsigned char>(buf[1]))
                    : 0;
                return RecvStatus::Close;
            }

            // Ping/pong: libcurl auto-replies to pings by default; just skip.
            if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;

            const bool finalChunk = !(meta->flags & CURLWS_CONT) && meta->bytesleft == 0;

            // Binary messages aren't used by this JSON gateway: discard them,
            // including any continuation fragments that follow.
            if (meta->flags & CURLWS_BINARY) skipping_binary_ = true;
            if (skipping_binary_) {
                if (finalChunk) skipping_binary_ = false;
                continue;
            }

            acc.append(buf, got);
            if (finalChunk) return RecvStatus::Message;
        }
    }

    // Waits up to `ms` for the socket to become readable. Call WITHOUT
    // holding any lock (it doesn't take one).
    void wait_readable(int ms) const {
        if (sock_ == CURL_SOCKET_BAD) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            return;
        }
#ifdef _WIN32
        WSAPOLLFD fd{};
        fd.fd = sock_;
        fd.events = POLLRDNORM;
        WSAPoll(&fd, 1, ms);
#else
        pollfd fd{};
        fd.fd = sock_;
        fd.events = POLLIN;
        poll(&fd, 1, ms);
#endif
    }

    const std::string& last_error() const { return last_error_; }

private:
    CURL* curl_{nullptr};
    curl_socket_t sock_{CURL_SOCKET_BAD};
    std::mutex mu_;
    char errbuf_[CURL_ERROR_SIZE] = {};
    std::string last_error_;
    bool skipping_binary_{false};
};

// ---------------------------------------------------------------------------
// RAII for the heartbeat thread
// ---------------------------------------------------------------------------
//
// Destructor runs on every exit path (normal break *or* exception unwinding),
// so a still-joinable std::thread is never destroyed (which would call
// std::terminate). Also wakes the thread's interruptible sleep so join()
// returns immediately instead of waiting out a full heartbeat interval.
class HeartbeatGuard {
public:
    HeartbeatGuard(std::thread& t,
                   std::atomic<bool>& stopFlag,
                   std::atomic<bool>& runningFlag,
                   std::mutex& m,
                   std::condition_variable& cv)
        : thread_(t), stop_(stopFlag), running_(runningFlag), m_(m), cv_(cv) {}
    ~HeartbeatGuard() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_.store(true);
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        running_.store(false);
    }
    HeartbeatGuard(const HeartbeatGuard&) = delete;
    HeartbeatGuard& operator=(const HeartbeatGuard&) = delete;

private:
    std::thread& thread_;
    std::atomic<bool>& stop_;
    std::atomic<bool>& running_;
    std::mutex& m_;
    std::condition_variable& cv_;
};

// ---------------------------------------------------------------------------
// Payload builders
// ---------------------------------------------------------------------------

// IDENTIFY payload (sent after HELLO). Never logged in full; the IDENTIFY-sent
// log line only logs a redacted token.
static std::string build_identify(const std::string& token) {
    nlohmann::json identify = {
        {"op", 2},
        {"d", {
            {"token", token},
            {"intents", 0},
            {"properties", {
                {"os", "windows"},   // arbitrary label for the gateway; change if you like
                {"browser", "fluxerpp"},
                {"device", "fluxerpp"}
            }}
        }}
    };
    return identify.dump();
}

static std::string build_resume(const std::string& token, const std::string& session_id, int seq) {
    nlohmann::json resume = {
        {"op", 6},
        {"d", {
            {"token", token},
            {"session_id", session_id},
            {"seq", seq}
        }}
    };
    return resume.dump();
}

// ---------------------------------------------------------------------------
// GatewayClient
// ---------------------------------------------------------------------------

GatewayClient::GatewayClient(const std::string& t)
    : token(t) { }

void GatewayClient::on_ready(const std::function<void()>& cb) {
    dispatcher.on_ready(cb);
}

void GatewayClient::on_message_create(const std::function<void(const models::Message&)>& cb) {
    dispatcher.on_message_create(cb);
}

void GatewayClient::on_guild_create(const std::function<void(const models::Guild&)>& cb) {
    dispatcher.on_guild_create(cb);
}

void GatewayClient::on_latency(const std::function<void(int)>& cb) {
    dispatcher.on_latency(cb);
}

void GatewayClient::on_heartbeat_ack(const std::function<void()>& cb) {
    dispatcher.on_heartbeat_ack(cb);
}

void GatewayClient::stop() {
    // The receive loop polls with a short timeout and checks this flag, so
    // connect() returns within ~100ms. No cross-thread handle closing needed.
    stop_requested_.store(true);
}

void GatewayClient::connect() {
    ensure_curl_init();

    constexpr int kPollMs = 100;

    int reconnectAttempt = 0;

    std::string session_id;
    int last_seq = -1;
    // Tracks whether the *server* told us the session is resumable (via
    // op 9 INVALID_SESSION's `d` field, or a 4009 close code).
    bool session_resumable = true;

    // Resolve the real gateway URL via GET /gateway/bot before dialing
    // anything, matching WebSocketManager.connect() in the JS client.
    ParsedWsUrl resolved{fallback_host, "/"};
    if (rest_) {
        try {
            nlohmann::json gw = rest_->get("/gateway/bot");
            std::string url = gw.at("url").get<std::string>();
            resolved = parse_ws_url(url);
            Logger::instance().info("Resolved gateway via /gateway/bot: " + url);
        } catch (const std::exception& ex) {
            Logger::instance().error(std::string("GET /gateway/bot failed: ") + ex.what() +
                                      " - falling back to " + fallback_host);
        }
    } else {
        Logger::instance().warn("No RestClient bound (call bind_rest()) - using fallback_host " +
                                 fallback_host + " instead of resolving /gateway/bot");
    }

    const std::string queryChar = (resolved.path.find('?') == std::string::npos) ? "?" : "&";
    const std::string fullUrl = resolved.scheme + "://" + resolved.host + resolved.path +
                                queryChar + "v=" + gateway_version + "&encoding=json";

    while (true) {
        if (stop_requested_.load()) {
            Logger::instance().info("stop() was called - not (re)connecting.");
            break;
        }

        bool connectionClosed = false;
        int closeStatus = 0;

        // Declared before the heartbeat machinery so it is destroyed AFTER the
        // heartbeat thread has been joined (reverse declaration order).
        WsConnection ws;
        std::string openErr;

        if (!ws.open(fullUrl, openErr)) {
            // Previously a failed connect returned from connect() entirely.
            // Now it goes through the normal backoff/retry path below so a
            // transient network outage at startup doesn't kill the client.
            Logger::instance().error("WebSocket connect failed: " + openErr);
            connectionClosed = true;
        } else {
            Logger::instance().info("Connected via libcurl WebSocket");

            std::string acc;

            std::atomic<int> heartbeat_interval_ms{0};
            std::atomic<bool> heartbeat_running{false};
            std::atomic<bool> stop_heartbeat{false};
            // Set by the heartbeat thread when it decides the connection is
            // dead (missed ACK / send failure). Replaces force-closing the
            // socket handle from another thread.
            std::atomic<bool> force_close{false};
            // Set true right after we send any heartbeat (scheduled or
            // server-requested), cleared on op 11 (HEARTBEAT ACK). If still
            // true when the next heartbeat is due, the previous ACK never
            // arrived.
            std::atomic<bool> awaiting_ack{false};
            std::mutex seq_mutex;
            std::mutex hb_mutex;
            std::condition_variable hb_cv;
            std::thread heartbeatThread;
            HeartbeatGuard heartbeatGuard(heartbeatThread, stop_heartbeat, heartbeat_running,
                                          hb_mutex, hb_cv);

            bool identified_or_resumed = false;
            const bool want_resume = !session_id.empty();

            while (!stop_requested_.load() && !force_close.load()) {
                int closeCode = 0;
                WsConnection::RecvStatus st = ws.recv_message(acc, closeCode);

                if (st == WsConnection::RecvStatus::Again) {
                    ws.wait_readable(kPollMs);
                    continue;
                }

                if (st == WsConnection::RecvStatus::Error) {
                    Logger::instance().warn("Receive failed: " + ws.last_error());
                    closeStatus = 0;
                    connectionClosed = true;
                    break;
                }

                if (st == WsConnection::RecvStatus::Close) {
                    closeStatus = closeCode;
                    Logger::instance().info("Received CLOSE frame, status=" + std::to_string(closeStatus));
                    connectionClosed = true;
                    break;
                }

                // st == Message: `acc` holds one fully reassembled text message.
                std::string js = std::move(acc);
                acc.clear();

                if (debug_logging_) {
                    Logger::instance().debug("RAW FRAME: " + js);
                }

                // The whole parse-through-dispatch sequence is wrapped: a
                // field with an unexpected type (e.g. "op" sent as a string)
                // makes nlohmann throw type_error. A malformed message is
                // logged and skipped, not fatal.
                try {
                    nlohmann::json data = nlohmann::json::parse(js);

                    if (data.contains("s") && !data["s"].is_null()) {
                        std::lock_guard<std::mutex> lk(seq_mutex);
                        last_seq = data.value("s", last_seq);
                    }

                    int op = data.value("op", -1);

                    if (debug_logging_) {
                        std::string t = data.value("t", std::string());
                        Logger::instance().debug("DISPATCH t=" + (t.empty() ? "<none>" : t) +
                                                  " op=" + std::to_string(op));
                    }

                    if (op == 10) { // HELLO
                        int interval = 0;
                        try { interval = data["d"].value("heartbeat_interval", 0); } catch (...) {}
                        heartbeat_interval_ms.store(interval);
                        Logger::instance().info("HELLO interval=" + std::to_string(interval));

                        if (!heartbeat_running.load()) {
                            heartbeat_running.store(true);
                            stop_heartbeat.store(false);
                            heartbeatThread = std::thread([&]() {
                                using namespace std::chrono;

                                // Sleep that wakes immediately when the guard
                                // requests shutdown.
                                auto interruptible_sleep = [&](int ms) {
                                    std::unique_lock<std::mutex> lk(hb_mutex);
                                    hb_cv.wait_for(lk, milliseconds(ms),
                                                   [&] { return stop_heartbeat.load(); });
                                };

                                int local_interval = heartbeat_interval_ms.load();
                                if (local_interval <= 0) local_interval = 41250;
                                interruptible_sleep(local_interval / 2);

                                while (!stop_heartbeat.load() &&
                                       !force_close.load() &&
                                       !stop_requested_.load()) {
                                    if (awaiting_ack.load()) {
                                        Logger::instance().warn("Missed heartbeat ACK - forcing reconnect");
                                        force_close.store(true);
                                        break;
                                    }

                                    int seq_snapshot;
                                    {
                                        std::lock_guard<std::mutex> lk(seq_mutex);
                                        seq_snapshot = last_seq;
                                    }
                                    nlohmann::json hb;
                                    hb["op"] = 1;
                                    hb["d"] = (seq_snapshot == -1) ? nlohmann::json(nullptr)
                                                                   : nlohmann::json(seq_snapshot);

                                    CURLcode sendRc = ws.send_text(hb.dump());
                                    if (sendRc != CURLE_OK) {
                                        Logger::instance().warn(std::string("Heartbeat send failed: ") +
                                                                curl_easy_strerror(sendRc));
                                        force_close.store(true);
                                        break;
                                    }
                                    // Timestamped right after the confirmed send so
                                    // serialization time doesn't count toward latency.
                                    last_hb_sent_.store(std::chrono::steady_clock::now());
                                    awaiting_ack.store(true);

                                    int sleep_ms = heartbeat_interval_ms.load();
                                    if (sleep_ms <= 0) sleep_ms = local_interval;
                                    interruptible_sleep(sleep_ms);
                                }
                            });
                        }

                        if (want_resume && !identified_or_resumed) {
                            std::string resumePayload = build_resume(this->token, session_id, last_seq);
                            CURLcode sendRc = ws.send_text(resumePayload);
                            if (sendRc == CURLE_OK) {
                                Logger::instance().info("Sent RESUME (session_id=" + session_id +
                                                        ", seq=" + std::to_string(last_seq) + ")");
                                identified_or_resumed = true;
                            } else {
                                Logger::instance().warn(std::string("RESUME send failed: ") +
                                                        curl_easy_strerror(sendRc) + " - will IDENTIFY");
                            }
                        }

                        if (!identified_or_resumed) {
                            std::string identifyPayload = build_identify(this->token);
                            CURLcode sendRc = ws.send_text(identifyPayload);
                            if (sendRc != CURLE_OK) {
                                Logger::instance().error(std::string("IDENTIFY send failed: ") +
                                                         curl_easy_strerror(sendRc));
                                connectionClosed = true;
                            } else {
                                Logger::instance().info("Sent IDENTIFY (token=" + Logger::redact(this->token) + ")");
                                identified_or_resumed = true;
                            }
                        }

                    } else if (op == 0) { // DISPATCH
                        std::string t = data.value("t", "");

                        if (t == "READY") {
                            try { session_id = data["d"].value("session_id", session_id); } catch (...) {}
                            // A successful READY means this attempt worked end to
                            // end - reset the backoff counter.
                            reconnectAttempt = 0;
                            session_resumable = true;
                            Logger::instance().info("READY received; session_id=" + session_id);
                            dispatcher.dispatch_ready();
                        } else if (t == "MESSAGE_CREATE") {
                            try {
                                models::Message msg = models::Message::from_data(data["d"], rest_);
                                dispatcher.dispatch_message_create(msg);
                            } catch (const std::exception& ex) {
                                Logger::instance().error(std::string("MESSAGE_CREATE handling failed: ") + ex.what());
                            }
                        } else if (t == "GUILD_CREATE") {
                            try {
                                models::Guild guild = models::Guild::from_data(data["d"], rest_);
                                dispatcher.dispatch_guild_create(guild);
                            } catch (const std::exception& ex) {
                                Logger::instance().error(std::string("GUILD_CREATE handling failed: ") + ex.what());
                            }
                        }
                        // other dispatch events: add routing here as needed

                    } else if (op == 1) { // Server heartbeat request
                        int seq_snapshot;
                        {
                            std::lock_guard<std::mutex> lk(seq_mutex);
                            seq_snapshot = last_seq;
                        }
                        nlohmann::json hb;
                        hb["op"] = 1;
                        hb["d"] = (seq_snapshot == -1) ? nlohmann::json(nullptr)
                                                       : nlohmann::json(seq_snapshot);

                        CURLcode sendRc = ws.send_text(hb.dump());
                        if (sendRc != CURLE_OK) {
                            Logger::instance().warn(std::string("Heartbeat (response) send failed: ") +
                                                    curl_easy_strerror(sendRc));
                            connectionClosed = true;
                        } else {
                            // Update last_hb_sent_ here too so the ACK for a
                            // server-requested heartbeat measures latency
                            // against THIS send, not the previous scheduled one.
                            last_hb_sent_.store(std::chrono::steady_clock::now());
                            awaiting_ack.store(true);
                        }

                    } else if (op == 7) { // RECONNECT
                        Logger::instance().info("Server requested reconnect (OP 7)");
                        // Deliberately does NOT touch session_resumable -
                        // RECONNECT always implies "come back and resume".
                        connectionClosed = true;

                    } else if (op == 9) { // INVALID SESSION
                        bool resumable = false;
                        try { resumable = data["d"].get<bool>(); } catch (...) {}
                        Logger::instance().warn(std::string("INVALID SESSION (resumable=") +
                                                (resumable ? "true" : "false") + ")");
                        session_resumable = resumable;
                        if (!resumable) {
                            session_id.clear();
                            last_seq = -1;
                        }
                        connectionClosed = true;

                    } else if (op == 11) { // HEARTBEAT ACK
                        awaiting_ack.store(false);
                        auto now = std::chrono::steady_clock::now();
                        auto sent = last_hb_sent_.load();
                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - sent).count();
                        dispatcher.dispatch_latency(static_cast<int>(ms));
                        dispatcher.dispatch_heartbeat_ack();
                        if (debug_logging_) Logger::instance().debug("Heartbeat ACK");
                    }

                } catch (const std::exception& ex) {
                    Logger::instance().warn(std::string("Failed to handle message: ") + ex.what());
                    continue;
                } catch (...) {
                    Logger::instance().warn("Failed to handle message: unknown exception");
                    continue;
                }

                if (connectionClosed) break;
            } // end receive loop

            // The heartbeat thread asked for a reconnect (missed ACK / send failure).
            if (force_close.load() && !stop_requested_.load()) {
                connectionClosed = true;
            }

            // Polite shutdown. Skipped for forced reconnects so the session
            // stays resumable (a 1000 close would invalidate it).
            if (stop_requested_.load()) {
                Logger::instance().info("Receive loop ending due to stop().");
                ws.send_close(1000);
            }

            // heartbeatGuard joins the heartbeat thread here, then `ws` is
            // destroyed when the enclosing scope ends. RAII on every exit path.
        }

        if (stop_requested_.load()) {
            break;
        }

        if (!connectionClosed) {
            Logger::instance().info("Connection ended without close status; stopping.");
            break;
        }

        if (closeStatus == 1000) {
            Logger::instance().info("Normal close (1000). Not reconnecting.");
            break;
        }
        if (closeStatus == 4009) {
            session_resumable = true; // server-side signal, in case op 9 didn't already tell us
        }

        reconnectAttempt++;
        if (reconnectAttempt > max_reconnect_attempts) {
            Logger::instance().error("Max reconnect attempts reached (" +
                                     std::to_string(max_reconnect_attempts) + "). Giving up.");
            break;
        }

        int backoffSeconds = (1 << (reconnectAttempt - 1));
        if (backoffSeconds > 30) backoffSeconds = 30;
        Logger::instance().info("Reconnecting in " + std::to_string(backoffSeconds) +
                                "s (attempt " + std::to_string(reconnectAttempt) + ")");

        // Sleep in small slices so stop() isn't stuck waiting out a 30s backoff.
        for (int waited = 0; waited < backoffSeconds * 1000 && !stop_requested_.load(); waited += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!session_resumable) {
            session_id.clear();
            last_seq = -1;
        } else if (!session_id.empty()) {
            Logger::instance().info("Attempting resume on reconnect (session_id=" + session_id +
                                    ", seq=" + std::to_string(last_seq) + ")");
        }
    } // end outer reconnect loop
}

} // namespace fluxerpp
