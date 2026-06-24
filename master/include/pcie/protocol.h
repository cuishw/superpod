#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pcie {

using HostId = std::uint32_t;

enum class RpcCode : std::int32_t {
    kOk = 0,
    kInvalidArgument = 1,
    kHostAlreadyRegistered = 2,
    kNotImplemented = 3,
    kInsufficientMemory = 4,
    kInvalidBlockId = 5,
    kBlockNotAllocated = 6,
    kHostNotRegistered = 7,
    kInvalidKey = 8,
    kKeyAlreadyExists = 9,
    kKeyNotFound = 10,
    kKeyHostMismatch = 11,
};

struct MemoryRegistration {
    HostId host_id{};
    std::uint64_t start_address{};
    std::uint64_t total_size{};
    std::uint64_t block_size{};

    bool operator==(const MemoryRegistration&) const = default;
};

struct RegisterMemoryResponse {
    RpcCode code{RpcCode::kOk};
    std::string message;
    std::uint64_t total_blocks{};
};

struct AllocBlocksRequest {
    HostId host_id{};
    std::vector<std::string> keys;
};

using BlockId = std::uint64_t;

struct KeyBlockLocation {
    std::string key;
    HostId host_id{};
    BlockId block_id{};
};

struct AllocBlocksResponse {
    RpcCode code{RpcCode::kOk};
    std::string message;
    std::vector<KeyBlockLocation> allocations;
};

struct FreeBlocksRequest {
    HostId host_id{};
    std::vector<std::string> keys;
};

struct FreeBlocksResponse {
    RpcCode code{RpcCode::kOk};
    std::string message;
    std::uint64_t freed_count{};
};

struct GetRequest {
    std::string key;
};

struct GetResponse {
    RpcCode code{RpcCode::kOk};
    std::string message;
    HostId host_id{};
    BlockId block_id{};
};

}  // namespace pcie
