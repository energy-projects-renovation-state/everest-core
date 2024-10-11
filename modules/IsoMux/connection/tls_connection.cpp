// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2023 Pionix GmbH and Contributors to EVerest

#include "tls_connection.hpp"
#include "connection.hpp"
#include "log.hpp"
#include "v2g.hpp"
#include "v2g_server.hpp"
#include <openssl/ssl.h>
#include <tls.hpp>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sys/types.h>
#include <thread>

namespace {

// used when ctx->network_read_timeout_tls is 0
constexpr int default_timeout_ms = 1000;

void process_connection_thread(std::shared_ptr<tls::ServerConnection> con, struct v2g_context* ctx) {
    assert(con != nullptr);
    assert(ctx != nullptr);

    openssl::pkey_ptr contract_public_key{nullptr, nullptr};
    auto connection = std::make_unique<v2g_connection>();
    connection->ctx = ctx;
    connection->is_tls_connection = true;
    connection->read = &tls::connection_read;
    connection->write = &tls::connection_write;
    connection->proxy = &tls::connection_proxy;
    connection->tls_connection = con.get();
    connection->pubkey = &contract_public_key;

    dlog(DLOG_LEVEL_INFO, "Incoming TLS connection");

    bool loop{true};
    while (loop) {
        loop = false;
        const auto result = con->accept();
        switch (result) {
        case tls::Connection::result_t::success:

            // TODO(james-ctc) v2g_ctx->tls_key_logging

            if (ctx->state == 0) {
                const auto rv = ::connection_handle(connection.get());
                dlog(DLOG_LEVEL_INFO, "connection_handle exited with %d", rv);
            } else {
                dlog(DLOG_LEVEL_INFO, "%s", "Closing tls-connection. v2g-session is already running");
            }

            con->shutdown();
            break;
        case tls::Connection::result_t::want_read:
        case tls::Connection::result_t::want_write:
            loop = con->wait_for(result, default_timeout_ms) == tls::Connection::result_t::success;
            break;
        case tls::Connection::result_t::closed:
        case tls::Connection::result_t::timeout:
        default:
            break;
        }
    }
}

void handle_new_connection_cb(tls::Server::ConnectionPtr&& con, struct v2g_context* ctx) {
    assert(con != nullptr);
    assert(ctx != nullptr);
    // create a thread to process this connection
    try {
        // passing unique pointers through thread parameters is problematic
        std::shared_ptr<tls::ServerConnection> connection(con.release());
        std::thread connection_loop(process_connection_thread, connection, ctx);
        connection_loop.detach();
    } catch (const std::system_error&) {
        // unable to start thread
        dlog(DLOG_LEVEL_ERROR, "pthread_create() failed: %s", strerror(errno));
        con->shutdown();
    }
}

void server_loop_thread(struct v2g_context* ctx) {
    assert(ctx != nullptr);
    assert(ctx->tls_server != nullptr);
    const auto res = ctx->tls_server->serve([ctx](auto con) { handle_new_connection_cb(std::move(con), ctx); });
    if (res != tls::Server::state_t::stopped) {
        dlog(DLOG_LEVEL_ERROR, "tls::Server failed to serve");
    }
}

bool build_config(tls::Server::config_t& config, struct v2g_context* ctx) {
    assert(ctx != nullptr);
    assert(ctx->r_security != nullptr);

    using types::evse_security::CaCertificateType;
    using types::evse_security::EncodingFormat;
    using types::evse_security::GetCertificateInfoStatus;
    using types::evse_security::LeafCertificateType;

    /*
     * libevse-security checks for an optional password and when one
     * isn't set is uses an empty string as the password rather than nullptr.
     * hence private keys are always encrypted.
     */

    bool bResult{false};

    config.cipher_list = "ECDHE-ECDSA-AES128-SHA256:ECDH-ECDSA-AES128-SHA256";
    config.ciphersuites = "";     // disable TLS 1.3
    config.verify_client = false; // contract certificate managed in-band in 15118-2

    // use the existing configured socket
    // TODO(james-ctc): switch to server socket init code otherwise there
    //                  may be issues with reinitialisation
    config.socket = ctx->tls_socket.fd;
    config.io_timeout_ms = static_cast<std::int32_t>(ctx->network_read_timeout_tls);

    // information from libevse-security
    const auto cert_info =
        ctx->r_security->call_get_leaf_certificate_info(LeafCertificateType::V2G, EncodingFormat::PEM, false);
    if (cert_info.status != GetCertificateInfoStatus::Accepted) {
        dlog(DLOG_LEVEL_ERROR, "Failed to read cert_info! Not Accepted");
    } else {
        if (cert_info.info) {
            const auto& info = cert_info.info.value();
            const auto cert_path = info.certificate.value_or("");
            const auto key_path = info.key;

            // workaround (see above libevse-security comment)
            const auto key_password = info.password.value_or("");

            auto& ref = config.chains.emplace_back();
            ref.certificate_chain_file = cert_path.c_str();
            ref.private_key_file = key_path.c_str();
            ref.private_key_password = key_password.c_str();

            if (info.ocsp) {
                for (const auto& ocsp : info.ocsp.value()) {
                    const char* file{nullptr};
                    if (ocsp.ocsp_path) {
                        file = ocsp.ocsp_path.value().c_str();
                    }
                    ref.ocsp_response_files.push_back(file);
                }
            }

            bResult = true;
        } else {
            dlog(DLOG_LEVEL_ERROR, "Failed to read cert_info! Empty response");
        }
    }

    return bResult;
}

tls::Server::OptionalConfig configure_ssl(struct v2g_context* ctx) {
    try {
        dlog(DLOG_LEVEL_WARNING, "configure_ssl");
        auto config = std::make_unique<tls::Server::config_t>();

        // The config of interest is from Evse Security, no point in updating
        // config when there is a problem

        if (build_config(*config, ctx)) {
            return {{std::move(config)}};
        }
    } catch (const std::bad_alloc&) {
        dlog(DLOG_LEVEL_ERROR, "unable to create TLS config");
    }
    return std::nullopt;
}

} // namespace

namespace tls {

int connection_init(struct v2g_context* ctx) {
    using state_t = tls::Server::state_t;

    assert(ctx != nullptr);
    assert(ctx->tls_server != nullptr);
    assert(ctx->r_security != nullptr);

    int res{-1};
    tls::Server::config_t config;

    // build_config can fail due to issues with Evse Security,
    // this can be retried later. Not treated as an error.
    (void)build_config(config, ctx);

    // apply config
    ctx->tls_server->stop();
    ctx->tls_server->wait_stopped();
    const auto result = ctx->tls_server->init(config, [ctx]() { return configure_ssl(ctx); });
    if ((result == state_t::init_complete) || (result == state_t::init_socket)) {
        res = 0;
    }

    return res;
}

int connection_start_server(struct v2g_context* ctx) {
    assert(ctx != nullptr);
    assert(ctx->tls_server != nullptr);

    // only starts the TLS server

    int res = 0;
    try {
        ctx->tls_server->stop();
        ctx->tls_server->wait_stopped();
        if (ctx->tls_server->state() == tls::Server::state_t::stopped) {
            // need to re-initialise
            tls::connection_init(ctx);
        }
        std::thread serve_loop(server_loop_thread, ctx);
        serve_loop.detach();
        ctx->tls_server->wait_running();
    } catch (const std::system_error&) {
        // unable to start thread (caller logs failure)
        res = -1;
    }
    return res;
}

ssize_t connection_read(struct v2g_connection* conn, unsigned char* buf, const std::size_t count, bool read_complete) {
    assert(conn != nullptr);
    assert(conn->tls_connection != nullptr);

    ssize_t result{0};
    std::size_t bytes_read{0};
    timespec ts_start{};

    if (clock_gettime(CLOCK_MONOTONIC, &ts_start) == -1) {
        dlog(DLOG_LEVEL_ERROR, "clock_gettime(ts_start) failed: %s", strerror(errno));
        result = -1;
    }

    while ((bytes_read < count) && (result >= 0)) {
        const std::size_t remaining = count - bytes_read;
        std::size_t bytes_in{0};
        auto* ptr = reinterpret_cast<std::byte*>(&buf[bytes_read]);

        const auto read_res = conn->tls_connection->read(ptr, remaining, bytes_in);
        switch (read_res) {
        case tls::Connection::result_t::success:
            bytes_read += bytes_in;
            break;
        case tls::Connection::result_t::want_read:
        case tls::Connection::result_t::want_write:
            conn->tls_connection->wait_for(read_res, default_timeout_ms);
            break;
        case tls::Connection::result_t::timeout:
            // the MBedTLS code loops on timeout, is_sequence_timeout() is used instead
            break;
        case tls::Connection::result_t::closed:
        default:
            result = -1;
            break;
        }

        if (conn->ctx->is_connection_terminated) {
            dlog(DLOG_LEVEL_ERROR, "Reading from tcp-socket aborted");
            conn->tls_connection->shutdown();
            result = -2;
        }

        if (::is_sequence_timeout(ts_start, conn->ctx)) {
            break;
        }

        if (not read_complete) {
            break;
        }
    }

    return (result < 0) ? result : static_cast<ssize_t>(bytes_read);
}

ssize_t connection_write(struct v2g_connection* conn, unsigned char* buf, std::size_t count) {
    assert(conn != nullptr);
    assert(conn->tls_connection != nullptr);

    ssize_t result{0};
    std::size_t bytes_written{0};

    while ((bytes_written < count) && (result >= 0)) {
        const std::size_t remaining = count - bytes_written;
        std::size_t bytes_out{0};
        const auto* ptr = reinterpret_cast<std::byte*>(&buf[bytes_written]);

        const auto write_res = conn->tls_connection->write(ptr, remaining, bytes_out);
        switch (write_res) {
        case tls::Connection::result_t::success:
            bytes_written += bytes_out;
            break;
        case tls::Connection::result_t::want_read:
        case tls::Connection::result_t::want_write:
            conn->tls_connection->wait_for(write_res, default_timeout_ms);
            break;
        case tls::Connection::result_t::timeout:
            // the MBedTLS code loops on timeout
            break;
        case tls::Connection::result_t::closed:
        default:
            result = -1;
            break;
        }
    }

    if ((result == -1) && (conn->tls_connection->state() == tls::Connection::state_t::closed)) {
        // if the connection has closed - return the number of bytes sent
        result = 0;
    }

    return (result < 0) ? result : static_cast<ssize_t>(bytes_written);
}

int connection_proxy(struct v2g_connection* conn, int proxy_fd) {

    dlog(DLOG_LEVEL_INFO, "Multiplexer: Proxy TLS->TCP");
    int ev_fd = conn->tls_connection->socket(); // underlying socket of TLS connection

    // SupportedAppProtocolReq message is still in buffer, we need to forward it to the external stack
    write(proxy_fd, conn->buffer, conn->payload_len + 8);

    struct pollfd poll_list[2];
    poll_list[0].fd = proxy_fd;
    poll_list[1].fd = ev_fd;
    poll_list[0].events = POLLIN;
    poll_list[1].events = POLLIN;

    unsigned char buf[2048];

    // Set reading to (more or less) non-blocking
    conn->tls_connection->set_read_timeout(10);

    while (true) {
        // Note we cannot simply poll on the underlying system socket for TLS connection
        // as it does not guarantee that SSL_read/write will not block after the poll
        // (an SSL_read my trigger an actual write or multiple reads on the system socket)
        // So we have to try a non-blocking SSL_read first, openssl will then tell us
        // what to wait for on the socket before we try again (read, write or both)

        auto r = conn->read(conn, buf, sizeof(buf), false);

        if (r < 0) {
            // something is wrong with the connection, exiting...
            break;
        } else if (r > 0) {
            // successfully read bytes, forward to proxy module
            write(proxy_fd, buf, r);
        }

        // check if SSL was actually waiting on write
        int e = SSL_get_error(conn->tls_connection->ssl_context(), r);
        if (e == SSL_ERROR_WANT_WRITE) {
            poll_list[1].events = POLLIN | POLLOUT;
        } else {
            poll_list[1].events = POLLIN;
        }

        int ret = poll(poll_list, 2, -1);

        if (ret == -1) {
            return -1; // poll error
        }

        // Timed out, but we blocked forever. This could be a spurious wakeup, so just try again.
        if (ret == 0) {
            continue;
        }

        if (poll_list[0].revents & POLLIN) {
            // we can read from proxy (connection to local ISO module)
            int nrbytes = read(proxy_fd, buf, sizeof(buf));

            if (nrbytes == 0) {
                break;
            }
            // write data to EV
            nrbytes = conn->write(conn, buf, nrbytes);
        }

        if (poll_list[0].revents & POLLERR or poll_list[0].revents & POLLHUP or poll_list[0].revents & POLLNVAL) {
            // something is wrong with the TCP connection to the ISO module
            return -1;
        }

        if (poll_list[1].revents & POLLIN or poll_list[1].revents & POLLOUT) {
            // we can read from / write to the EV raw socket, just continue here.
            // The actual SSL_read() will happen at the beginning of the loop
            continue;
        }

        if (poll_list[1].revents & POLLERR or poll_list[1].revents & POLLHUP or poll_list[1].revents & POLLNVAL) {
            // something is wrong with the TCP connection to the EV
            return -1;
        }
    }

    close(proxy_fd);
    return 0;
}

} // namespace tls