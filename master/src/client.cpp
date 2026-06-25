#include <charconv>
#include <csignal>
#include <cstdint>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

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
    std::string config_path;
    std::uint64_t block_size{4096};
};

void PrintProgramUsage(std::string_view program) {
    std::cout << "Usage: " << program
              << " --host-id ID [--host HOST] [--port PORT]\n"
              << "       [--config PATH] [--block-size BYTES]\n";
}

struct PhysmapConfigEntry {
    pcie::HostId host_id{};
    std::string device_path;
    std::uint64_t start_address{};
    std::uint64_t size{};
};

struct HostMapping {
    pcie::HostId host_id{};
    std::string device_path;
    std::uint64_t start_address{};
    std::uint64_t size{};
    int fd{-1};
    void* base{MAP_FAILED};

    HostMapping() = default;
    HostMapping(const HostMapping&) = delete;
    HostMapping& operator=(const HostMapping&) = delete;

    HostMapping(HostMapping&& other) noexcept {
        *this = std::move(other);
    }

    HostMapping& operator=(HostMapping&& other) noexcept {
        if (this != &other) {
            Close();
            host_id = other.host_id;
            device_path = std::move(other.device_path);
            start_address = other.start_address;
            size = other.size;
            fd = other.fd;
            base = other.base;
            other.fd = -1;
            other.base = MAP_FAILED;
        }
        return *this;
    }

    ~HostMapping() {
        Close();
    }

    void Close() {
        if (base != MAP_FAILED) {
            munmap(base, static_cast<std::size_t>(size));
            base = MAP_FAILED;
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
};

class HostMappings {
   public:
    bool Load(const std::string& config_path);

    [[nodiscard]] const PhysmapConfigEntry* FindConfig(
        pcie::HostId host_id) const;
    [[nodiscard]] const HostMapping* FindMapping(pcie::HostId host_id) const;
    [[nodiscard]] bool empty() const { return mappings_.empty(); }

   private:
    std::vector<PhysmapConfigEntry> configs_;
    std::unordered_map<pcie::HostId, HostMapping> mappings_;
};

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
        << "      Return Block IDs for the host with the most ordered matches.\n"
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

bool HasExtraToken(std::istringstream& input);

std::optional<std::uint64_t> BlockOffset(pcie::BlockId block_id,
                                         std::uint64_t block_size) {
    if (block_size != 0 &&
        block_id > std::numeric_limits<std::uint64_t>::max() / block_size) {
        return std::nullopt;
    }
    return block_id * block_size;
}

std::optional<const void*> BlockAddress(const HostMappings& mappings,
                                        const pcie::KeyBlockLocation& location,
                                        std::uint64_t block_size) {
    const auto* mapping = mappings.FindMapping(location.host_id);
    if (mapping == nullptr) {
        return std::nullopt;
    }
    const auto offset = BlockOffset(location.block_id, block_size);
    if (!offset || *offset >= mapping->size) {
        return std::nullopt;
    }
    return static_cast<const std::byte*>(mapping->base) + *offset;
}

bool ParsePhysmapConfigLine(const std::string& line, std::size_t line_number,
                            PhysmapConfigEntry& entry) {
    std::istringstream input(line);
    std::string host_id_text;
    std::string start_text;
    std::string size_text;
    if (!(input >> host_id_text >> entry.device_path >> start_text >> size_text) ||
        HasExtraToken(input)) {
        std::cerr << "Invalid physmap config line " << line_number
                  << ": expected <host_id> <char_device> <start_address> <size>\n";
        return false;
    }

    std::uint64_t host_id = 0;
    if (!ParseUnsigned(host_id_text, host_id) ||
        host_id > std::numeric_limits<pcie::HostId>::max() ||
        !ParseUnsigned(start_text, entry.start_address) ||
        !ParseUnsigned(size_text, entry.size) || entry.size == 0) {
        std::cerr << "Invalid numeric value in physmap config line "
                  << line_number << '\n';
        return false;
    }
    entry.host_id = static_cast<pcie::HostId>(host_id);
    return true;
}

bool HostMappings::Load(const std::string& config_path) {
    std::ifstream file(config_path);
    if (!file) {
        std::cerr << "Failed to open physmap config " << config_path << ": "
                  << std::strerror(errno) << '\n';
        return false;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        std::istringstream blank_check(line);
        std::string first_token;
        if (!(blank_check >> first_token)) {
            continue;
        }

        PhysmapConfigEntry entry;
        if (!ParsePhysmapConfigLine(line, line_number, entry)) {
            return false;
        }
        if (mappings_.contains(entry.host_id)) {
            std::cerr << "Duplicate host ID " << entry.host_id
                      << " in physmap config\n";
            return false;
        }

        HostMapping mapping;
        mapping.host_id = entry.host_id;
        mapping.device_path = entry.device_path;
        mapping.start_address = entry.start_address;
        mapping.size = entry.size;
        mapping.fd = open(entry.device_path.c_str(), O_RDWR | O_SYNC);
        if (mapping.fd < 0) {
            std::cerr << "Failed to open " << entry.device_path << ": "
                      << std::strerror(errno) << '\n';
            return false;
        }
        mapping.base = mmap(nullptr, static_cast<std::size_t>(entry.size),
                            PROT_READ | PROT_WRITE, MAP_SHARED, mapping.fd, 0);
        if (mapping.base == MAP_FAILED) {
            std::cerr << "Failed to mmap " << entry.device_path << " size 0x"
                      << std::hex << entry.size << std::dec << ": "
                      << std::strerror(errno) << '\n';
            return false;
        }

        configs_.push_back(entry);
        mappings_.emplace(entry.host_id, std::move(mapping));
    }

    if (mappings_.empty()) {
        std::cerr << "Physmap config contains no mappings: "
                  << config_path << '\n';
        return false;
    }
    return true;
}

const PhysmapConfigEntry* HostMappings::FindConfig(pcie::HostId host_id) const {
    for (const auto& config : configs_) {
        if (config.host_id == host_id) {
            return &config;
        }
    }
    return nullptr;
}

const HostMapping* HostMappings::FindMapping(pcie::HostId host_id) const {
    const auto mapping = mappings_.find(host_id);
    if (mapping == mappings_.end()) {
        return nullptr;
    }
    return &mapping->second;
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
        } else if (argument == "--config") {
            options.config_path = value;
        } else if (argument == "--block-size") {
            if (!ParseUnsigned(value, options.block_size) ||
                options.block_size == 0) {
                std::cerr << "Invalid block size: " << value << '\n';
                return false;
            }
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

bool SendRegisterMemory(coro_rpc::coro_rpc_client& client,
                        const pcie::MemoryRegistration& request) {
    auto result = async_simple::coro::syncAwait(
        client.call<&pcie::MasterService::RegisterMemory>(request));
    if (!result) {
        std::cerr << "RPC failed: " << result.error().msg << '\n';
        return false;
    }

    const auto& response = result.value();
    std::cout << "RegisterMemory: " << CodeName(response.code)
              << ", message=\"" << response.message << "\""
              << ", total_blocks=" << response.total_blocks << '\n';
    return response.code == pcie::RpcCode::kOk;
}

bool RegisterConfiguredHost(coro_rpc::coro_rpc_client& client,
                            const HostMappings& mappings,
                            pcie::HostId host_id,
                            std::uint64_t block_size) {
    const auto* config = mappings.FindConfig(host_id);
    if (config == nullptr) {
        std::cerr << "No physmap config entry for --host-id "
                  << host_id << '\n';
        return false;
    }
    std::cout << "Registering configured host " << host_id
              << " from " << config->device_path
              << " start=0x" << std::hex << config->start_address
              << " size=0x" << config->size
              << " block_size=0x" << block_size << std::dec << '\n';
    return SendRegisterMemory(
        client, {host_id, config->start_address, config->size, block_size});
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
    static_cast<void>(SendRegisterMemory(client, request));
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

void PrintBlockAddress(const HostMappings& mappings,
                       const pcie::KeyBlockLocation& location,
                       std::uint64_t block_size) {
    const auto address = BlockAddress(mappings, location, block_size);
    if (address) {
        std::cout << ", mmap_addr=" << *address;
        return;
    }
    if (!mappings.empty()) {
        std::cout << ", mmap_addr=unmapped";
    }
}

void ExistBlock(coro_rpc::coro_rpc_client& client,
                std::istringstream& input,
                const HostMappings& mappings,
                std::uint64_t block_size) {
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
        PrintBlockAddress(
            mappings,
            {"", response.host_id, response.block_id},
            block_size);
    }
    std::cout << '\n';
}

void BatchExistBlocks(coro_rpc::coro_rpc_client& client,
                      std::istringstream& input,
                      const HostMappings& mappings,
                      std::uint64_t block_size) {
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
                  << match.host_id << ':' << match.block_id;
        PrintBlockAddress(mappings, match, block_size);
        std::cout << '\n';
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
                    pcie::HostId host_id,
                    const HostMappings& mappings,
                    std::uint64_t block_size) {
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
            ExistBlock(client, input, mappings, block_size);
        } else if (command == "batch_exist") {
            BatchExistBlocks(client, input, mappings, block_size);
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

    HostMappings mappings;
    if (!options.config_path.empty()) {
        if (!mappings.Load(options.config_path)) {
            return 1;
        }
        if (!RegisterConfiguredHost(client, mappings, options.host_id,
                                    options.block_size)) {
            return 1;
        }
    }

    std::cout << "Connected\n";
    std::cout << "Client host ID: " << options.host_id
              << " (fixed for this process)\n";
    if (!mappings.empty()) {
        std::cout << "Mapped physmap hosts from " << options.config_path
                  << "; block_size=" << options.block_size
                  << ". CPU must not dereference these addresses directly; "
                     "use the GPU/DMA path.\n";
    }
    RunInteractive(client, options.host_id, mappings, options.block_size);
    return 0;
}
