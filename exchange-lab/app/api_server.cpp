#include "router.hpp"

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <charconv>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using exchange::Engine;
namespace api = exchange::api;

constexpr int max_body = 16 * 1024;

struct Server {
    Engine engine;
    std::string address;
    std::string localhost;
};

void send(evhttp_request* request, const api::Response& response) {
    const std::unique_ptr<evbuffer, decltype(&evbuffer_free)> buffer{
        evbuffer_new(), &evbuffer_free};
    auto* headers = evhttp_request_get_output_headers(request);
    if (!buffer ||
        evbuffer_add(buffer.get(), response.body.data(), response.body.size()) <
            0 ||
        evhttp_add_header(headers, "Content-Type",
                          "application/json; charset=utf-8") < 0 ||
        evhttp_add_header(headers, "Cache-Control", "no-store") < 0 ||
        (response.allow &&
         evhttp_add_header(headers, "Allow", response.allow) < 0)) {
        throw std::runtime_error{"could not construct HTTP response"};
    }
    evhttp_send_reply(request, response.status, nullptr, buffer.get());
}

void internal_error(evhttp_request* request) noexcept {
    try {
        send(request, api::error(500, "internal_error",
                                 "request could not be completed"));
    } catch (...) {
        evhttp_send_error(request, 500, nullptr);
    }
}

std::string_view method(evhttp_cmd_type type) {
    switch (type) {
        case EVHTTP_REQ_GET:
            return "GET";
        case EVHTTP_REQ_POST:
            return "POST";
        default:
            return {};
    }
}

void handle(evhttp_request* request, void* context) noexcept {
    auto& server = *static_cast<Server*>(context);
    try {
        auto* headers = evhttp_request_get_input_headers(request);
        const auto* host = evhttp_find_header(headers, "Host");
        if (!host || (server.address != host && server.localhost != host)) {
            send(request, api::error(400, "invalid_host",
                                     "use the server's loopback address"));
            return;
        }
        const auto* uri = evhttp_request_get_evhttp_uri(request);
        const auto* path = uri ? evhttp_uri_get_path(uri) : nullptr;
        if (!path || evhttp_uri_get_query(uri)) {
            send(request,
                 api::error(400, "invalid_request",
                            "expected a path without query parameters"));
            return;
        }
        auto* input = evhttp_request_get_input_buffer(request);
        const auto size = evbuffer_get_length(input);
        if (size > max_body) {
            send(request, api::error(413, "payload_too_large",
                                     "request body exceeds 16 KiB"));
            return;
        }
        std::string_view body;
        if (size != 0) {
            const auto* data =
                evbuffer_pullup(input, static_cast<ev_ssize_t>(size));
            if (!data) throw std::bad_alloc{};
            body = {reinterpret_cast<const char*>(data), size};
        }
        const auto* content_type = evhttp_find_header(headers, "Content-Type");
        send(request, api::route(server.engine,
                                 method(evhttp_request_get_command(request)),
                                 path, content_type ? content_type : "", body));
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "HTTP request failed: %s\n", ex.what());
        internal_error(request);
    } catch (...) {
        internal_error(request);
    }
}

void stop(evutil_socket_t, short, void* base) {
    event_base_loopbreak(static_cast<event_base*>(base));
}

unsigned short port_argument(int argc, char** argv) {
    if (argc == 1) return 8080;
    if (argc == 2) {
        const std::string_view value{argv[1]};
        const auto end = value.data() + value.size();
        unsigned short port{};
        const auto parsed = std::from_chars(value.data(), end, port);
        if (parsed.ec == std::errc{} && parsed.ptr == end) return port;
    }
    throw std::invalid_argument{"usage: exchange_api [port]"};
}

void serve(unsigned short port) {
    Server server;
    const std::unique_ptr<event_base, decltype(&event_base_free)> base{
        event_base_new(), &event_base_free};
    if (!base) throw std::runtime_error{"could not start event loop"};
    const std::unique_ptr<evhttp, decltype(&evhttp_free)> http{
        evhttp_new(base.get()), &evhttp_free};
    if (!http) throw std::runtime_error{"could not start HTTP server"};

    evhttp_set_gencb(http.get(), handle, &server);
    evhttp_set_allowed_methods(
        http.get(), EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD |
                        EVHTTP_REQ_PUT | EVHTTP_REQ_DELETE |
                        EVHTTP_REQ_OPTIONS | EVHTTP_REQ_PATCH);
    evhttp_set_max_headers_size(http.get(), 16 * 1024);
    evhttp_set_max_body_size(http.get(), max_body);
    evhttp_set_timeout(http.get(), 10);

    const auto* host = "127.0.0.1";
    auto* bound = evhttp_bind_socket_with_handle(http.get(), host, port);
    if (!bound)
        throw std::runtime_error{"could not bind 127.0.0.1:" +
                                 std::to_string(port)};
    sockaddr_in address{};
    socklen_t size = sizeof(address);
    if (getsockname(evhttp_bound_socket_get_fd(bound),
                    reinterpret_cast<sockaddr*>(&address), &size) < 0) {
        throw std::runtime_error{"could not read bound port"};
    }

    const auto suffix = ntohs(address.sin_port) == 80
                            ? std::string{}
                            : ":" + std::to_string(ntohs(address.sin_port));
    server.address = host + suffix;
    server.localhost = "localhost" + suffix;

    const std::unique_ptr<event, decltype(&event_free)> interrupt{
        evsignal_new(base.get(), SIGINT, stop, base.get()), &event_free};
    const std::unique_ptr<event, decltype(&event_free)> terminate{
        evsignal_new(base.get(), SIGTERM, stop, base.get()), &event_free};
    if (!interrupt || !terminate || event_add(interrupt.get(), nullptr) < 0 ||
        event_add(terminate.get(), nullptr) < 0) {
        throw std::runtime_error{"could not register shutdown handlers"};
    }
    std::cout << "Listening on http://" << host << ':'
              << ntohs(address.sin_port) << std::endl;
    if (event_base_dispatch(base.get()) < 0)
        throw std::runtime_error{"event loop failed"};
}

}  // namespace

int main(int argc, char** argv) {
    try {
        serve(port_argument(argc, argv));
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
