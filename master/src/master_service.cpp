#include "pcie/master_service.h"

#include <cctype>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pcie {
namespace {

RegisterMemoryResponse Invalid(std::string message) {
    return {RpcCode::kInvalidArgument, std::move(message), 0};
}

bool IsSha256Key(std::string_view key) {
    if (key.size() != 64) {
        return false;
    }
    for (const auto character : key) {
        if (!std::isxdigit(static_cast<unsigned char>(character))) {
            return false;
        }
    }
    return true;
}

std::string CanonicalKey(std::string_view key) {
    std::string canonical(key);
    for (auto& character : canonical) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return canonical;
}

template <typename Response>
std::optional<Response> ValidateKeys(const std::vector<std::string>& keys) {
    if (keys.empty()) {
        return Response{RpcCode::kInvalidArgument,
                        "at least one SHA-256 key is required", {}};
    }

    std::unordered_set<std::string> unique_keys;
    unique_keys.reserve(keys.size());
    for (const auto& key : keys) {
        if (!IsSha256Key(key)) {
            return Response{RpcCode::kInvalidKey,
                            "key must be exactly 64 hexadecimal characters",
                            {}};
        }
        if (!unique_keys.insert(CanonicalKey(key)).second) {
            return Response{RpcCode::kInvalidArgument,
                            "the request contains a duplicate key", {}};
        }
    }
    return std::nullopt;
}

}  // namespace

std::uint64_t MasterService::HostMemory::AvailableBlockCount() const {
    return total_blocks - static_cast<std::uint64_t>(allocated_blocks.size());
}

std::uint64_t MasterService::HostMemory::AllocateOne() {
    std::uint64_t block_index = 0;
    if (next_fresh_block < total_blocks) {
        block_index = next_fresh_block++;
    } else {
        block_index = recycled_blocks.front();
        recycled_blocks.pop_front();
    }
    allocated_blocks.insert(block_index);
    return block_index;
}

void MasterService::HostMemory::FreeOne(std::uint64_t block_index) {
    allocated_blocks.erase(block_index);
    recycled_blocks.push_back(block_index);
}

RegisterMemoryResponse MasterService::RegisterMemory(
    const MemoryRegistration& request) {
    if (request.total_size == 0) {
        return Invalid("total_size must be greater than zero");
    }
    if (request.block_size == 0) {
        return Invalid("block_size must be greater than zero");
    }
    if (request.total_size % request.block_size != 0) {
        return Invalid("total_size must be a multiple of block_size");
    }
    if (request.start_address % request.block_size != 0) {
        return Invalid("start_address must be aligned to block_size");
    }
    if (request.start_address >
        std::numeric_limits<std::uint64_t>::max() - request.total_size) {
        return Invalid("memory address range overflows uint64_t");
    }

    const auto total_blocks = request.total_size / request.block_size;
    std::unique_lock lock(mutex_);
    const auto existing = hosts_.find(request.host_id);
    if (existing != hosts_.end()) {
        if (existing->second.registration == request) {
            return {RpcCode::kOk, "host already registered with identical memory", total_blocks};
        }
        return {RpcCode::kHostAlreadyRegistered,
                "host_id is already registered with different memory", 0};
    }

    if (available_blocks_ >
        std::numeric_limits<std::uint64_t>::max() - total_blocks) {
        return Invalid("total registered block count overflows uint64_t");
    }

    HostMemory host;
    host.registration = request;
    host.total_blocks = total_blocks;
    hosts_.emplace(request.host_id, std::move(host));
    available_blocks_ += total_blocks;
    return {RpcCode::kOk, "memory registered", total_blocks};
}

AllocBlocksResponse MasterService::AllocBlocks(const AllocBlocksRequest& request) {
    if (const auto error = ValidateKeys<AllocBlocksResponse>(request.keys)) {
        return *error;
    }

    std::unique_lock lock(mutex_);
    const auto host_position = hosts_.find(request.host_id);
    if (host_position == hosts_.end()) {
        return {RpcCode::kHostNotRegistered,
                "host_id is not registered", {}};
    }

    auto& host = host_position->second;
    for (const auto& key : request.keys) {
        if (key_index_.contains(CanonicalKey(key))) {
            return {RpcCode::kKeyAlreadyExists,
                    "one or more keys are already allocated", {}};
        }
    }

    const auto block_count = static_cast<std::uint64_t>(request.keys.size());
    if (block_count > host.AvailableBlockCount()) {
        return {RpcCode::kInsufficientMemory,
                "not enough free blocks in the requested host pool", {}};
    }

    // Reserve all growable containers before changing allocation state.
    key_index_.reserve(key_index_.size() + request.keys.size());
    host.allocated_blocks.reserve(host.allocated_blocks.size() +
                                  request.keys.size());
    std::vector<KeyBlockLocation> allocations;
    allocations.reserve(request.keys.size());
    for (const auto& key : request.keys) {
        auto canonical_key = CanonicalKey(key);
        const auto block_id = host.AllocateOne();
        key_index_.emplace(canonical_key,
                           BlockLocation{request.host_id, block_id});
        allocations.push_back(
            {std::move(canonical_key), request.host_id, block_id});
    }

    available_blocks_ -= block_count;
    return {RpcCode::kOk, "blocks allocated", std::move(allocations)};
}

FreeBlocksResponse MasterService::FreeBlocks(const FreeBlocksRequest& request) {
    if (const auto error = ValidateKeys<FreeBlocksResponse>(request.keys)) {
        return *error;
    }

    std::unique_lock lock(mutex_);
    const auto host_position = hosts_.find(request.host_id);
    if (host_position == hosts_.end()) {
        return {RpcCode::kHostNotRegistered,
                "host_id is not registered", 0};
    }
    auto& host = host_position->second;

    // Validate the complete request first so FreeBlocks is all-or-nothing.
    std::vector<BlockId> block_ids;
    block_ids.reserve(request.keys.size());
    for (const auto& key : request.keys) {
        const auto location = key_index_.find(CanonicalKey(key));
        if (location == key_index_.end()) {
            return {RpcCode::kKeyNotFound, "key is not allocated", 0};
        }
        if (location->second.host_id != request.host_id) {
            return {RpcCode::kKeyHostMismatch,
                    "key belongs to a different host", 0};
        }
        block_ids.push_back(location->second.block_id);
    }

    for (std::size_t index = 0; index < request.keys.size(); ++index) {
        host.FreeOne(block_ids[index]);
        key_index_.erase(CanonicalKey(request.keys[index]));
    }
    available_blocks_ += static_cast<std::uint64_t>(request.keys.size());
    return {RpcCode::kOk, "blocks freed",
            static_cast<std::uint64_t>(request.keys.size())};
}

GetResponse MasterService::Get(const GetRequest& request) const {
    if (!IsSha256Key(request.key)) {
        return {RpcCode::kInvalidKey,
                "key must be exactly 64 hexadecimal characters", 0, 0};
    }

    std::shared_lock lock(mutex_);
    const auto location = key_index_.find(CanonicalKey(request.key));
    if (location == key_index_.end()) {
        return {RpcCode::kKeyNotFound, "key is not allocated", 0, 0};
    }
    return {RpcCode::kOk, "key found", location->second.host_id,
            location->second.block_id};
}

std::optional<MemoryRegistration> MasterService::FindHost(HostId host_id) const {
    std::shared_lock lock(mutex_);
    const auto host = hosts_.find(host_id);
    if (host == hosts_.end()) {
        return std::nullopt;
    }
    return host->second.registration;
}

std::size_t MasterService::RegisteredHostCount() const {
    std::shared_lock lock(mutex_);
    return hosts_.size();
}

std::uint64_t MasterService::AvailableBlockCount() const {
    std::shared_lock lock(mutex_);
    return available_blocks_;
}

std::uint64_t MasterService::AllocatedBlockCount() const {
    std::shared_lock lock(mutex_);
    std::uint64_t allocated = 0;
    for (const auto& [host_id, host] : hosts_) {
        static_cast<void>(host_id);
        allocated += static_cast<std::uint64_t>(host.allocated_blocks.size());
    }
    return allocated;
}

}  // namespace pcie
