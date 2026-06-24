#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "pcie/protocol.h"

namespace pcie {

class MasterService {
   public:
    RegisterMemoryResponse RegisterMemory(const MemoryRegistration& request);

    AllocBlocksResponse AllocBlocks(const AllocBlocksRequest& request);
    FreeBlocksResponse FreeBlocks(const FreeBlocksRequest& request);
    GetResponse Get(const GetRequest& request) const;

    [[nodiscard]] std::optional<MemoryRegistration> FindHost(HostId host_id) const;
    [[nodiscard]] std::size_t RegisteredHostCount() const;
    [[nodiscard]] std::uint64_t AvailableBlockCount() const;
    [[nodiscard]] std::uint64_t AllocatedBlockCount() const;

   private:
    struct HostMemory {
        MemoryRegistration registration;
        std::uint64_t total_blocks{};
        std::uint64_t next_fresh_block{};
        std::deque<std::uint64_t> recycled_blocks;
        std::unordered_set<std::uint64_t> allocated_blocks;

        [[nodiscard]] std::uint64_t AvailableBlockCount() const;
        std::uint64_t AllocateOne();
        void FreeOne(std::uint64_t block_index);
    };

    struct BlockLocation {
        HostId host_id{};
        BlockId block_id{};
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<HostId, HostMemory> hosts_;
    std::unordered_map<std::string, BlockLocation> key_index_;
    std::uint64_t available_blocks_{};
};

}  // namespace pcie
