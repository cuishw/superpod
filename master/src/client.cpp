#include <charconv>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include <async_simple/coro/SyncAwait.h>
#include <ylt/coro_rpc/coro_rpc_client.hpp>

#include "pcie/master_service.h"
#include "pcie/line_editor.h"

namespace {

struct Options {
    std::string host{"127.0.0.1"};
    std::uint16_t port{50051};
    pcie::HostId host_id{};
    bool has_host_id{false};
};

void PrintProgramUsage(std::string_view program) {
    std::cout << "Usage: " << program
              << " --host-id ID [--host HOST] [--port PORT]\n";
}

void PrintCommands() {
    std::cout
        << "Commands:\n"
        << "  register <start_address> <total_size> <block_size>\n"
        << "      Register a memory region. Numbers may be decimal or 0x-prefixed hex.\n"
        << "  alloc <sha256_key> [sha256_key ...]\n"
        << "      Allocate one block for each key in this client's host pool.\n"
        << "  free <sha256_key> [sha256_key ...]\n"
        << "      Free this client's blocks by key.\n"
        << "  exist <sha256_key>\n"
        << "      Find the host ID and Block ID assigned to a key.\n"
        << "  batch_exist <sha256_key> [sha256_key ...]\n"
        << "      Find the longest matching prefix whose blocks belong to one host.\n"
        << "  help\n"
        << "  quit | exit\n";
}

bool ParseUnsigned(std::string_view text, std::uint64_t& value) {
    int base = 10;
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return false;
    }

    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value, base);
    return result.ec == std::errc{} && result.ptr == end;
}

bool ParseOptions(int argc, char* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            PrintProgramUsage(argv[0]);
            return false;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return false;
        }

        const std::string_view value(argv[++index]);
        if (argument == "--host") {
            options.host = value;
        } else if (argument == "--port") {
            std::uint64_t port = 0;
            if (!ParseUnsigned(value, port) || port == 0 || port > 65535) {
                std::cerr << "Invalid port: " << value << '\n';
                return false;
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (argument == "--host-id") {
            if (options.has_host_id) {
                std::cerr << "--host-id may only be specified once\n";
                return false;
            }
            std::uint64_t host_id = 0;
            if (!ParseUnsigned(value, host_id) ||
                host_id > std::numeric_limits<pcie::HostId>::max()) {
                std::cerr << "Invalid host ID: " << value << '\n';
                return false;
            }
            options.host_id = static_cast<pcie::HostId>(host_id);
            options.has_host_id = true;
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            return false;
        }
    }
    if (!options.has_host_id) {
        std::cerr << "Missing required option: --host-id\n";
        return false;
    }
    return true;
}

std::string_view CodeName(pcie::RpcCode code) {
    switch (code) {
        case pcie::RpcCode::kOk:
            return "OK";
        case pcie::RpcCode::kInvalidArgument:
            return "INVALID_ARGUMENT";
        case pcie::RpcCode::kHostAlreadyRegistered:
            return "HOST_ALREADY_REGISTERED";
        case pcie::RpcCode::kNotImplemented:
            return "NOT_IMPLEMENTED";
        case pcie::RpcCode::kInsufficientMemory:
            return "INSUFFICIENT_MEMORY";
        case pcie::RpcCode::kInvalidBlockId:
            return "INVALID_BLOCK_ID";
        case pcie::RpcCode::kBlockNotAllocated:
            return "BLOCK_NOT_ALLOCATED";
        case pcie::RpcCode::kHostNotRegistered:
            return "HOST_NOT_REGISTERED";
        case pcie::RpcCode::kInvalidKey:
            return "INVALID_KEY";
        case pcie::RpcCode::kKeyAlreadyExists:
            return "KEY_ALREADY_EXISTS";
        case pcie::RpcCode::kKeyNotFound:
            return "KEY_NOT_FOUND";
        case pcie::RpcCode::kKeyHostMismatch:
            return "KEY_HOST_MISMATCH";
    }
    return "UNKNOWN";
}

bool HasExtraToken(std::istringstream& input) {
    std::string extra;
    return static_cast<bool>(input >> extra);
}

void RegisterMemory(coro_rpc::coro_rpc_client& client,
                    std::istringstream& input, pcie::HostId host_id) {
    std::string address_text;
    std::string total_size_text;
    std::string block_size_text;
    if (!(input >> address_text >> total_size_text >> block_size_text) ||
        HasExtraToken(input)) {
        std::cerr << "Usage: register <start_address> <total_size> "
                     "<block_size>\n";
        return;
    }

    pcie::MemoryRegistration request;
    if (!ParseUnsigned(address_text, request.start_address) ||
        !ParseUnsigned(total_size_text, request.total_size) ||
        !ParseUnsigned(block_size_text, request.block_size)) {
        std::cerr << "Invalid numeric argument\n";
        return;
    }
    request.host_id = host_id;

    auto result = async_simple::coro::syncAwait(
        client.call<&pcie::MasterService::RegisterMemory>(request));
    if (!result) {
        std::cerr << "RPC failed: " << result.error().msg << '\n';
        return;
    }

    const auto& response = result.value();
    std::cout << "RegisterMemory: " << CodeName(response.code)
              << ", message=\"" << response.message << "\""
              << ", total_blocks=" << response.total_blocks << '\n';
}

void AllocBlocks(coro_rpc::coro_rpc_client& client,
                 std::istringstream& input, pcie::HostId host_id) {
    pcie::AllocBlocksRequest request;
    request.host_id = host_id;
    std::string key;
    while (input >> key) {
        request.keys.push_back(key);
    }
    if (request.keys.empty()) {
        std::cerr << "Usage: alloc <sha256_key> [sha256_key ...]\n";
        return;
    }

    auto result = async_simple::coro::syncAwait(
        client.call<&pcie::MasterService::AllocBlocks>(request));
    if (!result) {
        std::cerr << "RPC failed: " << result.error().msg << '\n';
        return;
    }

    const auto& response = result.value();
    std::cout << "AllocBlocks: " << CodeName(response.code)
              << ", message=\"" << response.message << "\""
              << ", host_id=" << host_id
              << ", blocks=" << response.allocations.size() << '\n';
    for (const auto& allocation : response.allocations) {
        std::cout << "  " << allocation.key << " -> "
                  << allocation.host_id << ':' << allocation.block_id << '\n';
    }
}

void ExistBlock(coro_rpc::coro_rpc_client& client,
                std::istringstream& input) {
    std::string key;
    if (!(input >> key) || HasExtraToken(input)) {
        std::cerr << "Usage: exist <sha256_key>\n";
        return;
    }

    auto result = async_simple::coro::syncAwait(
        client.call<&pcie::MasterService::Exist>(pcie::ExistRequest{key}));
    if (!result) {
        std::cerr << "RPC failed: " << result.error().msg << '\n';
        return;
    }

    const auto& response = result.value();
    std::cout << "Exist: " << CodeName(response.code)
              << ", message=\"" << response.message << "\"";
    if (response.code == pcie::RpcCode::kOk) {
        std::cout << ", host_id=" << response.host_id
                  << ", block_id=" << response.block_id;
    }
    std::cout << '\n';
}

void BatchExistBlocks(coro_rpc::coro_rpc_client& client,
                      std::istringstream& input) {
    pcie::BatchExistRequest request;
    std::string key;
    while (input >> key) {
        request.keys.push_back(key);
    }
    if (request.keys.empty()) {
        std::cerr << "Usage: batch_exist <sha256_key> [sha256_key ...]\n";
        return;
    }

    auto result = async_simple::coro::syncAwait(
        client.call<&pcie::MasterService::BatchExist>(request));
    if (!result) {
        std::cerr << "RPC failed: " << result.error().msg << '\n';
        return;
    }

    const auto& response = result.value();
    std::cout << "BatchExist: " << CodeName(response.code)
              << ", message=\"" << response.message << "\""
              << ", host_id=" << response.host_id
              << ", matched_count=" << response.matched_count << '\n';
    for (const auto& match : response.matches) {
        std::cout << "  " << match.key << " -> "
                  << match.host_id << ':' << match.block_id << '\n';
    }
}

void FreeBlocks(coro_rpc::coro_rpc_client& client,
                std::istringstream& input, pcie::HostId host_id) {
    pcie::FreeBlocksRequest request;
    request.host_id = host_id;
    std::string key;
    while (input >> key) {
        request.keys.push_back(key);
    }
    if (request.keys.empty()) {
        std::cerr << "Usage: free <sha256_key> [sha256_key ...]\n";
        return;
    }

    auto result = async_simple::coro::syncAwait(
        client.call<&pcie::MasterService::FreeBlocks>(request));
    if (!result) {
        std::cerr << "RPC failed: " << result.error().msg << '\n';
        return;
    }

    const auto& response = result.value();
    std::cout << "FreeBlocks: " << CodeName(response.code)
              << ", message=\"" << response.message << "\""
              << ", host_id=" << host_id
              << ", freed_count=" << response.freed_count << '\n';
}

void RunInteractive(coro_rpc::coro_rpc_client& client,
                    pcie::HostId host_id) {
    PrintCommands();
    pcie::LineEditor editor;
    while (true) {
        auto result = editor.ReadLine("rpc> ");
        if (result.status == pcie::ReadLineStatus::kEndOfInput) {
            return;
        }
        if (result.status == pcie::ReadLineStatus::kCancelled) {
            continue;
        }

        std::istringstream input(result.line);
        std::string command;
        if (!(input >> command)) {
            continue;
        }
        if (command == "register") {
            RegisterMemory(client, input, host_id);
        } else if (command == "alloc") {
            AllocBlocks(client, input, host_id);
        } else if (command == "free") {
            FreeBlocks(client, input, host_id);
        } else if (command == "exist") {
            ExistBlock(client, input);
        } else if (command == "batch_exist") {
            BatchExistBlocks(client, input);
        } else if (command == "help") {
            PrintCommands();
        } else if (command == "quit" || command == "exit") {
            return;
        } else {
            std::cerr << "Unknown command: " << command
                      << " (type 'help')\n";
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        return argc == 2 && (std::string_view(argv[1]) == "--help" ||
                             std::string_view(argv[1]) == "-h")
                   ? 0
                   : 2;
    }

    // Ctrl+C cancels the current interactive input. It must not terminate the
    // Client while it is connecting or waiting for an RPC response either.
    std::signal(SIGINT, SIG_IGN);

    coro_rpc::coro_rpc_client client;
    std::cout << "Connecting to " << options.host << ':' << options.port
              << "...\n";
    const auto error = async_simple::coro::syncAwait(
        client.connect(options.host, std::to_string(options.port)));
    if (error) {
        std::cerr << "Connection failed: " << error.message() << '\n';
        return 1;
    }

    std::cout << "Connected\n";
    std::cout << "Client host ID: " << options.host_id
              << " (fixed for this process)\n";
    RunInteractive(client, options.host_id);
    return 0;
}
