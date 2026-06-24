#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "pcie/master_service.h"

namespace {

void Check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void CheckBlock(pcie::BlockId block_id, std::uint64_t expected,
                std::string_view message) {
    Check(block_id == expected, message);
}

std::string Key(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(64) << value;
    return output.str();
}

std::vector<std::string> Keys(std::uint64_t first, std::size_t count) {
    std::vector<std::string> keys;
    keys.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        keys.push_back(Key(first + index));
    }
    return keys;
}
}  // namespace

int main() {
    pcie::MasterService service;
    const pcie::MemoryRegistration host1{1, 0x100000, 0x400000, 0x1000};
    const pcie::MemoryRegistration host2{2, 0x800000, 0x800000, 0x2000};

    const auto first = service.RegisterMemory(host1);
    Check(first.code == pcie::RpcCode::kOk, "first host registration");
    Check(first.total_blocks == 1024, "first host block count");

    const auto second = service.RegisterMemory(host2);
    Check(second.code == pcie::RpcCode::kOk, "second host registration");
    Check(service.RegisteredHostCount() == 2, "registrations are keyed by host ID");
    Check(service.FindHost(2) == host2, "host lookup returns its registration");

    const auto repeated = service.RegisterMemory(host1);
    Check(repeated.code == pcie::RpcCode::kOk, "identical registration is idempotent");
    Check(service.RegisteredHostCount() == 2, "idempotent registration adds no host");

    auto conflicting = host1;
    conflicting.total_size *= 2;
    const auto conflict = service.RegisterMemory(conflicting);
    Check(conflict.code == pcie::RpcCode::kHostAlreadyRegistered,
          "conflicting registration is rejected");

    const pcie::MemoryRegistration invalid{3, 0x1000, 100, 64};
    Check(service.RegisterMemory(invalid).code == pcie::RpcCode::kInvalidArgument,
          "non-integral block count is rejected");

    const auto allocation = service.AllocBlocks({1, Keys(1, 8)});
    Check(allocation.code == pcie::RpcCode::kOk, "AllocBlocks succeeds");
    Check(allocation.allocations.size() == 8,
          "AllocBlocks returns every Block ID");
    Check(service.AvailableBlockCount() == 2040,
          "allocation decreases the global free count");
    Check(service.AllocatedBlockCount() == 8,
          "allocation increases the allocated count");

    // Use small pools to verify host isolation and FIFO recycling.
    pcie::MasterService allocator;
    Check(allocator.RegisterMemory({10, 0x1000, 4 * 0x1000, 0x1000}).code ==
              pcie::RpcCode::kOk,
          "small host 10 registration");
    Check(allocator.RegisterMemory({20, 0x10000, 2 * 0x1000, 0x1000}).code ==
              pcie::RpcCode::kOk,
          "small host 20 registration");

    const auto host_10_keys = Keys(100, 6);
    const auto host_20_keys = Keys(200, 2);
    const auto first_alloc = allocator.AllocBlocks(
        {10, {host_10_keys[0], host_10_keys[1], host_10_keys[2]}});
    Check(first_alloc.code == pcie::RpcCode::kOk,
          "first small allocation succeeds");
    CheckBlock(first_alloc.allocations[0].block_id, 0, "first Block ID");
    CheckBlock(first_alloc.allocations[1].block_id, 1, "second Block ID");
    CheckBlock(first_alloc.allocations[2].block_id, 2, "third Block ID");

    Check(allocator.AllocBlocks({10, {host_10_keys[3], host_10_keys[4]}}).code ==
              pcie::RpcCode::kInsufficientMemory,
          "allocation cannot borrow blocks from another host pool");
    const auto host_20 = allocator.AllocBlocks({20, host_20_keys});
    Check(host_20.code == pcie::RpcCode::kOk,
          "host 20 allocates from its own pool");
    CheckBlock(host_20.allocations[0].block_id, 0, "host 20 first Block ID");
    CheckBlock(host_20.allocations[1].block_id, 1, "host 20 second Block ID");
    const auto host_10_last = allocator.AllocBlocks({10, {host_10_keys[3]}});
    CheckBlock(host_10_last.allocations[0].block_id, 3,
               "host 10 last Block ID");
    Check(allocator.AvailableBlockCount() == 0, "all small blocks allocated");

    Check(allocator.AllocBlocks({10, {host_10_keys[4]}}).code ==
              pcie::RpcCode::kInsufficientMemory,
          "allocation fails atomically when memory is exhausted");
    Check(allocator.AllocBlocks({10, {}}).code == pcie::RpcCode::kInvalidArgument,
          "allocation without keys is rejected");
    Check(allocator.AllocBlocks({10, {"bad-key"}}).code ==
              pcie::RpcCode::kInvalidKey,
          "non-SHA-256 key is rejected");
    Check(allocator.AllocBlocks({99, {Key(300)}}).code ==
              pcie::RpcCode::kHostNotRegistered,
          "allocation for an unregistered host is rejected");
    Check(allocator.AllocBlocks({20, {host_10_keys[1]}}).code ==
              pcie::RpcCode::kKeyAlreadyExists,
          "a key cannot be allocated twice or moved to another host");

    const auto found = allocator.Get({host_10_keys[1]});
    Check(found.code == pcie::RpcCode::kOk && found.host_id == 10 &&
              found.block_id == 1,
          "Get resolves a key to its host and Block ID");
    Check(allocator.Get({Key(999)}).code == pcie::RpcCode::kKeyNotFound,
          "Get reports an unknown SHA-256 key");
    Check(allocator.Get({"bad-key"}).code == pcie::RpcCode::kInvalidKey,
          "Get validates SHA-256 key format");

    const auto freed = allocator.FreeBlocks(
        {10, {host_10_keys[2], host_10_keys[0]}});
    Check(freed.code == pcie::RpcCode::kOk && freed.freed_count == 2,
          "FreeBlocks returns blocks to the pool");
    const auto recycled = allocator.AllocBlocks(
        {10, {host_10_keys[4], host_10_keys[5]}});
    CheckBlock(recycled.allocations[0].block_id, 2,
               "first freed ID leaves the FIFO first");
    CheckBlock(recycled.allocations[1].block_id, 0,
               "second freed ID leaves the FIFO second");

    const auto duplicate = allocator.FreeBlocks(
        {10, {host_10_keys[4], host_10_keys[4]}});
    Check(duplicate.code == pcie::RpcCode::kInvalidArgument,
          "duplicate keys are rejected");
    Check(allocator.AllocatedBlockCount() == 6,
          "failed FreeBlocks does not partially change state");

    const auto invalid_free = allocator.FreeBlocks(
        {10, {host_10_keys[4], Key(999)}});
    Check(invalid_free.code == pcie::RpcCode::kKeyNotFound,
          "unknown key is rejected atomically");
    Check(allocator.FreeBlocks({20, {host_10_keys[4]}}).code ==
              pcie::RpcCode::kKeyHostMismatch,
          "a key cannot be freed through another host pool");
    Check(allocator.FreeBlocks({99, {host_10_keys[4]}}).code ==
              pcie::RpcCode::kHostNotRegistered,
          "free for an unregistered host is rejected");
    Check(allocator.FreeBlocks({10, {host_10_keys[4]}}).code ==
              pcie::RpcCode::kOk,
          "valid key remains allocated after atomic validation failure");
    Check(allocator.Get({host_10_keys[4]}).code == pcie::RpcCode::kKeyNotFound,
          "freed key is removed from the global index");
    Check(allocator.FreeBlocks({10, {host_10_keys[4]}}).code ==
              pcie::RpcCode::kKeyNotFound,
          "double free is rejected");
    Check(allocator.FreeBlocks({10, {}}).code == pcie::RpcCode::kInvalidArgument,
          "empty free request is rejected");

    std::cout << "All MasterService tests passed\n";
    return EXIT_SUCCESS;
}
