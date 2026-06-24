#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <async_simple/coro/SyncAwait.h>
#include <ylt/coro_rpc/coro_rpc_client.hpp>
#include <ylt/coro_rpc/coro_rpc_server.hpp>

#include "pcie/master_service.h"

namespace {

void Check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    pcie::MasterService service;
    coro_rpc::coro_rpc_server server(2, 0, "127.0.0.1");
    server.register_handler<&pcie::MasterService::RegisterMemory>(&service);
    server.register_handler<&pcie::MasterService::AllocBlocks>(&service);
    server.register_handler<&pcie::MasterService::FreeBlocks>(&service);
    server.register_handler<&pcie::MasterService::Get>(&service);

    auto server_future = server.async_start();
    Check(!server_future.hasResult(), "RPC server starts asynchronously");

    coro_rpc::coro_rpc_client client;
    const auto connect_error = async_simple::coro::syncAwait(
        client.connect("127.0.0.1", std::to_string(server.port())));
    Check(!connect_error, "client connects to Master over TCP");

    const pcie::MemoryRegistration registration{
        7, 0x100000, 0x400000, 0x1000};
    const auto register_result = async_simple::coro::syncAwait(
        client.call<&pcie::MasterService::RegisterMemory>(registration));
    Check(static_cast<bool>(register_result), "RegisterMemory RPC succeeds");
    Check(register_result.value().code == pcie::RpcCode::kOk,
          "Master accepts the registration");
    Check(register_result.value().total_blocks == 1024,
          "Master returns the registered block count");
    Check(service.FindHost(7) == registration,
          "Master stores the network registration by host ID");

    const std::vector<std::string> keys{
        std::string(64, 'a'), std::string(64, 'b'),
        std::string(64, 'c'), std::string(64, 'd')};
    const auto alloc_result = async_simple::coro::syncAwait(
        client.call<&pcie::MasterService::AllocBlocks>(
            pcie::AllocBlocksRequest{7, keys}));
    Check(static_cast<bool>(alloc_result), "AllocBlocks RPC succeeds");
    Check(alloc_result.value().code == pcie::RpcCode::kOk,
          "Master allocates blocks");
    Check(alloc_result.value().allocations.size() == 4,
          "AllocBlocks returns all Block IDs");
    Check(alloc_result.value().allocations.front().block_id == 0,
          "Block IDs are local indexes in the requested host pool");

    coro_rpc::coro_rpc_client querying_client;
    const auto query_connect_error = async_simple::coro::syncAwait(
        querying_client.connect("127.0.0.1", std::to_string(server.port())));
    Check(!query_connect_error, "another node connects to Master");
    const auto cross_node_get = async_simple::coro::syncAwait(
        querying_client.call<&pcie::MasterService::Get>(
            pcie::GetRequest{std::string(64, 'A')}));
    Check(cross_node_get.value().code == pcie::RpcCode::kOk &&
              cross_node_get.value().host_id == 7 &&
              cross_node_get.value().block_id == 0,
          "another node resolves a key using case-insensitive SHA-256 hex");

    const auto get_result = async_simple::coro::syncAwait(
        client.call<&pcie::MasterService::Get>(pcie::GetRequest{keys[1]}));
    Check(static_cast<bool>(get_result), "Get RPC succeeds");
    Check(get_result.value().code == pcie::RpcCode::kOk &&
              get_result.value().host_id == 7 &&
              get_result.value().block_id == 1,
          "Get returns the key's host and Block ID");

    const auto free_result = async_simple::coro::syncAwait(
        client.call<&pcie::MasterService::FreeBlocks>(
            pcie::FreeBlocksRequest{7, {keys[0], keys[1]}}));
    Check(static_cast<bool>(free_result), "FreeBlocks RPC succeeds");
    Check(free_result.value().code == pcie::RpcCode::kOk &&
              free_result.value().freed_count == 2,
          "Master frees the requested Block IDs");
    Check(service.AvailableBlockCount() == 1022,
          "network free updates the allocator state");

    const auto get_freed = async_simple::coro::syncAwait(
        client.call<&pcie::MasterService::Get>(pcie::GetRequest{keys[1]}));
    Check(get_freed.value().code == pcie::RpcCode::kKeyNotFound,
          "Get no longer resolves a freed key");

    server.stop();
    std::cout << "All network RPC tests passed\n";
    return EXIT_SUCCESS;
}
