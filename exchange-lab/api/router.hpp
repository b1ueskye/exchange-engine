#ifndef EXCHANGE_API_ROUTER_HPP
#define EXCHANGE_API_ROUTER_HPP

#include "exchange/engine.hpp"

#include <string_view>

namespace exchange::api {

struct Response {
    int status;
    std::string body;
    const char* allow = nullptr;
};

Response error(int status, const char* code, std::string_view detail);
Response route(Engine& engine, std::string_view method, std::string_view path,
               std::string_view content_type, std::string_view body);

}  // namespace exchange::api

#endif
