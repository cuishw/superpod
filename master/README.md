# PCIe Memory Master

这是从原项目中提取的最小 Master。它沿用 `yalantinglibs/coro_rpc`，但不依赖
原项目的 CXL allocator、日志、指标或命令行库，因此可以在本目录独立构建。

## 当前 RPC

- `RegisterMemory(MemoryRegistration)`：按 `host_id` 保存主机的起始地址、总容量和
  BlockSize。完全相同的重复注册是幂等操作；同一 ID 的不同注册会被拒绝。
- `AllocBlocks(AllocBlocksRequest)`：请求携带固定的 `host_id` 和 SHA-256 key
  列表，每个 key 分配一个 Block，并返回 key、主机 ID 与 Block ID 的对应关系。
- `FreeBlocks(FreeBlocksRequest)`：请求携带固定的 `host_id` 和 key 列表，通过
  key 找到 Block 后将其放回对应主机空闲队列尾部。
- `Exist(ExistRequest)`：任何 Client 都可以查询指定 key，返回其所在的 `host_id`
  和 `block_id`。
- `BatchExist(BatchExistRequest)`：按请求顺序查询一组 key，遇到缺失 key 时立即
  停止，并在已匹配 key 涉及的多个 `host_id` 中返回匹配最多的主机及其
  Block ID 列表和匹配个数；返回的 Block 必须都属于同一个 `host_id`。

协议和 RPC 声明位于 `include/pcie/`，后续 Client 应复用这些头文件调用 RPC。
注册信息目前仅保存在 Master 进程内存中，Master 重启后需要 Client 重新注册。

## Block 管理策略

每个主机维护独立的 Block 池，Block ID 是主机内从 0 开始的索引。Alloc 和 Free
请求都携带 Client 启动时固定的 `host_id`，因此 Master 始终在对应池中操作，不会
跨主机借用 Block。Master 不会在注册时为全部内存创建 Block 数组，而是通过
`next_fresh_block` 懒生成从未分配过的 ID；当前已分配 ID
保存在哈希集合中，释放的 ID 追加到 FIFO 回收队列尾部。只有从未分配的 ID
用尽后，才从回收队列头部重新分配，因此释放顺序和再次分配顺序一致。

- 注册：O(1)，元数据大小不随注册内存容量增长。
- 分配：平均 O(申请 Block 数)，主机池定位平均 O(1)。
- 释放：O(key 数量)，平均 O(1) 查询每个 key；整批请求原子成功或失败。
- 批量查询：O(key 数量)，平均 O(1) 查询每个 key；结果只包含匹配最多的同一
  主机下的 Block。
- 元数据空间：O(当前已分配 Block 数 + 已回收待复用 Block 数)。

key 必须是恰好 64 个十六进制字符组成的 SHA-256 字符串，并在 Master 中全局
唯一。Master 维护 `key -> (host_id, block_id)` 哈希索引；Alloc 会拒绝已存在的
key，Free 只能释放属于请求 `host_id` 的 key，Exist/BatchExist 则允许其他节点
跨主机查询。Alloc 和 Free 都先校验完整批次，任一 key 出错时不会部分修改分配状态。

## 依赖

- 支持 C++20 的编译器
- CMake 3.16+
- yalantinglibs 0.5.1（与原项目使用的版本一致，并已执行 `cmake --install`）

可先在原项目根目录执行 `./dependencies.sh` 安装依赖，也可以单独安装
yalantinglibs 0.5.1。

## 独立构建与测试

以下命令的工作目录均为 `pcie`：

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

如果 yalantinglibs 安装在自定义前缀，请添加
`-DCMAKE_PREFIX_PATH=/path/to/prefix`。

## 运行

先启动 Master：

```bash
./build/pcie_master --address 0.0.0.0 --port 50051 --threads 4
```

使用 `./build/pcie_master --help` 查看参数。

另开一个终端启动交互式 Client：

```bash
./build/pcie_client --host-id 1 --host 127.0.0.1 --port 50051
```

也可以让 Client 读取 physmap 配置文件并自动注册当前 `--host-id` 对应的内存：

```bash
./build/pcie_client --host-id 2 --config physmap.conf --block-size 0x1000
```

配置文件每行格式为 `<host_id> <char_device> <start_address> <size>`，数字支持十进制
或 `0x` 前缀十六进制，例如：

```text
0 /dev/physmap0 0x0000008000000000 0x0000008000000000
1 /dev/physmap2 0x0000420000000000 0x0000008000000000
2 /dev/physmap3 0x0000440000000000 0x0000008000000000
3 /dev/physmap4 0x0000460000000000 0x0000008000000000
```

Client 会打开并 `mmap` 文件中的每个字符设备，用 `--host-id` 对应行向 Master
注册 `start_address`、`size` 和 `--block-size`。`exist`/`batch_exist` 返回远端
Block 时，Client 会按 `mmap_base + block_id * block_size` 打印对应映射地址；
这些 physmap 内存不能由 CPU 直接访问，应通过沐曦 GPU/DMA 路径使用。

交互示例：

```text
rpc> register 0x100000 0x400000 0x1000
RegisterMemory: OK, message="memory registered", total_blocks=1024
rpc> alloc aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
AllocBlocks: OK, message="blocks allocated", host_id=1, blocks=2
  aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa -> 1:0
  bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb -> 1:1
rpc> exist aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
Exist: OK, message="key found", host_id=1, block_id=0
rpc> batch_exist aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
BatchExist: OK, message="best host matches found", host_id=1, matched_count=2
  aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa -> 1:0
  bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb -> 1:1
rpc> free aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
FreeBlocks: OK, message="blocks freed", host_id=1, freed_count=1
rpc> quit
```

Client 支持上下方向键选择历史命令、左右方向键移动光标，以及 Home、End、
Backspace、Delete。按 `Ctrl+C` 只会取消当前输入并显示新的 `rpc>` 提示符，
不会终止 Client；使用 `quit`、`exit` 或在空行按 `Ctrl+D` 正常退出。
