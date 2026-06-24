#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <ylt/coro_rpc/coro_rpc_server.hpp>

#include "pcie/master_service.h"

namespace {

struct Options {
    std::string address{"0.0.0.0"};
    std::uint16_t port{50051};
    std::size_t threads{4};
};

void PrintUsage(std::string_view program) {
    std::cout << "Usage: " << program
              << " [--address ADDRESS] [--port PORT] [--threads COUNT]\n";
}
template <typename T>
bool ParseInteger(std::string_view text, T& value) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool ParseOptions(int argc, char* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]);
            return false;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return false;
        }

        const std::string_view value(argv[++index]);
        if (argument == "--address") {
            options.address = value;
        } else if (argument == "--port") {
            std::uint32_t port = 0;
            if (!ParseInteger(value, port) || port == 0 || port > 65535) {
                std::cerr << "Invalid port: " << value << '\n';
                return false;
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (argument == "--threads") {
            if (!ParseInteger(value, options.threads) || options.threads == 0) {
                std::cerr << "Invalid thread count: " << value << '\n';
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        return argc > 1 && (std::string_view(argv[1]) == "--help" ||
                            std::string_view(argv[1]) == "-h")
                   ? 0
                   : 2;
    }

    pcie::MasterService service;
    coro_rpc::coro_rpc_server server(
        options.threads, options.port, options.address,
        std::chrono::seconds(0), true);
    server.register_handler<&pcie::MasterService::RegisterMemory>(&service);
    server.register_handler<&pcie::MasterService::AllocBlocks>(&service);
    server.register_handler<&pcie::MasterService::FreeBlocks>(&service);
    server.register_handler<&pcie::MasterService::Get>(&service);

    std::cout << "PCIe memory master listening on " << options.address << ':'
              << options.port << " with " << options.threads << " RPC threads\n";
    return server.start();
}
