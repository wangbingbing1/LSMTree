# LSMtree 函数接口文档

本文档记录了 LSMtree 项目核心模块中每个函数/方法的目的、作用及实现思路，便于代码维护。

---

## 1. `binary_io.h` —— 二进制序列化

| 函数                                   | 目的                              | 作用                                | 实现思路                                                     |
| -------------------------------------- | --------------------------------- | ----------------------------------- | ------------------------------------------------------------ |
| `WriteBinary<T>(ostream&, const T&)`   | 将 POD/平凡可拷贝类型写入二进制流 | 直接 `write` 对象的原始字节         | 通过 `std::enable_if` 和 `std::is_trivially_copyable` 限制类型，避开 `std::string`，直接写入 `sizeof(T)` 字节。 |
| `WriteBinary(ostream&, const string&)` | 写入字符串                        | 先写 4 字节长度，再写字符数据       | 调用 `WriteBinary` 写入 `uint32_t` 长度，再调用 `os.write(str.data(), len)`。 |
| `ReadBinary<T>(istream&, T&)`          | 从二进制流读取平凡可拷贝类型      | 直接 `read` 填充对象                | 类似写入，限制类型为 `std::is_trivially_copyable`，读取 `sizeof(T)` 字节。 |
| `ReadBinary(istream&, string&)`        | 读取字符串                        | 先读 4 字节长度，再读指定长度的字符 | 调用 `ReadBinary` 读取长度，`str.resize(len)` 后 `read` 填入。 |
| `WriteFlags(ostream&, bool)`           | 写入墓碑标记                      | 将布尔值编码为一个字节              | 构造 `uint8_t` 变量（0或1），调用 `WriteBinary` 写入。       |
| `ReadFlags(istream&)`                  | 读取墓碑标记                      | 返回布尔值                          | 读取一个 `uint8_t`，通过 `&1` 提取最低位。                   |

---

## 2. `crc32.h` —— CRC32 校验

| 函数                         | 目的                    | 作用                    | 实现思路                                                     |
| ---------------------------- | ----------------------- | ----------------------- | ------------------------------------------------------------ |
| `CRC32(const char*, size_t)` | 计算数据的 CRC32 校验值 | 用于 WAL 记录完整性校验 | 使用预先生成的 CRC32 查表（多项式 0xEDB88320），逐字节迭代计算，初始值 `0xFFFFFFFF`，最后异或 `0xFFFFFFFF`。 |

---

## 3. `bloom_filter.h` —— 布隆过滤器

| 函数/方法                                  | 目的             | 作用                         | 实现思路                                                     |
| ------------------------------------------ | ---------------- | ---------------------------- | ------------------------------------------------------------ |
| `BloomFilter(bits_per_key, expected_keys)` | 构造函数         | 初始化位数组和哈希函数个数   | 根据 `bits_per_key * expected_keys` 计算位数（最小64），根据最优公式 `k = (bits/n) * ln2` 计算哈希函数个数，限制在 [1,30]。 |
| `Add(const K&)`                            | 插入键           | 将键对应的位设为 1           | 使用双哈希 `h1=hash(key)`, `h2=hash(key)+扰动`，生成 k 个位置 `(h1 + i*h2) % bits_.size()`，置位。 |
| `MayContain(const K&)`                     | 查询键可能存在性 | 检查所有对应位是否都为 1     | 同样计算 k 个位置，若任一位为 0 返回 false，否则返回 true（可能有假阳性）。 |
| `Serialize()`                              | 序列化           | 将布隆过滤器转为二进制字符串 | 写入 `k_`（int）、`bit_len`（size_t），然后将 `vector<bool>` 压缩为字节数组（每8位一个字节），追加到字符串。 |
| `Deserialize(const string&)`               | 反序列化         | 从二进制串重建布隆过滤器     | 读取 `k_`、`bit_len`，校验长度，然后逐位还原 `bits_`（每个字节的每一位）。 |
| `hash(const K&)`                           | 内部双哈希生成   | 返回两个独立的哈希值         | 使用 `std::hash<K>` 得到 h1，h2 = h1 + 0x9e3779b97f4a7c15ULL（黄金比例扰动）。 |

---

## 4. `skip_list.h` —— 内存表与 WAL

| 函数/方法                                           | 目的               | 作用                           | 实现思路                                                     |
| --------------------------------------------------- | ------------------ | ------------------------------ | ------------------------------------------------------------ |
| `SkipList::Insert(const K&, const V&)`              | 插入或更新键值对   | 写入内存表，非墓碑             | 加独占锁，调用 `InsertUnlocked`。                            |
| `SkipList::Get(const K&, V&)`                       | 查询有效值         | 忽略墓碑，返回 true 表示存在   | 加共享锁，从最高层向下查找，定位到底层节点，检查非墓碑后赋值。 |
| `SkipList::Remove(const K&)`                        | 删除键             | 插入墓碑标记                   | 加独占锁，若已存在墓碑直接返回，否则调用 `InsertUnlocked` 插入墓碑节点。 |
| `SkipList::ApplyBatch(const WriteBatch&, ostream&)` | 原子提交批处理     | 先序列化到 WAL，再应用到内存表 | 调用 `batch.SerializeToWAL(wal)`，然后加锁逐个应用操作。     |
| `SkipList::Iterator` 类                             | 遍历跳表           | 提供顺序迭代器                 | 构造时获取共享锁，保存头节点的 `forward[0]` 指针，移动时沿底层链表前进。 |
| `WritePutToWAL` / `WriteDeleteToWAL`                | 写入单条记录到 WAL | 持久化操作                     | 构造二进制缓冲区（type, key_len, key, value_len, value），计算 CRC32 并追加，写入流。 |

---

## 5. `block_cache.h` —— LRU 缓存

| 函数/方法                         | 目的               | 作用                                   | 实现思路                                                     |
| --------------------------------- | ------------------ | -------------------------------------- | ------------------------------------------------------------ |
| `Insert(const string&, const V&)` | 插入或更新缓存     | 淘汰最久未使用条目                     | 加锁，计算值大小；若键已存在，更新值并移动到 LRU 链表头部；否则先淘汰直到有空间，再插入头部。 |
| `Lookup(const string&, V&)`       | 查找缓存           | 命中则返回 true 并输出值，同时更新 LRU | 加锁，查找哈希表，若存在则将该键移动到链表头部，返回缓存值。 |
| `Evict()`                         | 淘汰最久未使用条目 | 删除链表尾部条目                       | 加锁（由 Insert 内调用），取 `lru_list_.back()`，从哈希表删除，更新总字节数。 |
| `EstimateSize` 标签分发           | 计算值占用的内存   | 用于容量控制                           | 对 `std::string` 返回 `val.size()`，其他类型返回 `sizeof(T)`。 |

---

## 6. `sstable.h` —— SSTable 文件

| 函数/方法                        | 目的                   | 作用                           | 实现思路                                                     |
| -------------------------------- | ---------------------- | ------------------------------ | ------------------------------------------------------------ |
| `SSTable::Build(filename, data)` | 构建 SSTable 文件      | 将有序键值对写入磁盘           | 写入数据区（flags, key, value），构建内存索引 `map<K,offset>`；写入索引区（条目数，key，offset）；构建布隆过滤器并写入；写入 Footer（index_offset, bloom_offset）。 |
| `SSTable::Get(key, value)`       | 点查询                 | 使用布隆过滤器和索引快速读取值 | 先检查布隆过滤器，再查找索引；若命中，从 `file_stream_` 定位到偏移，读取 flags 和 key，若非墓碑则读取 value 并写入缓存。 |
| `SSTable::Iterator::ReadNext()`  | 读取下一条记录         | 内部实现迭代器前进             | 从 `current_offset_` 读取 flags、key、value（如果非墓碑），更新 `key_`、`value_`、`tombstone_`，设置下一个偏移。 |
| `FlushMemTableToSSTable`         | 将内存表刷写到 SSTable | 持久化内存数据                 | 收集 `db` 所有条目，生成新 SSTable 文件，加入管理器，清空内存表和 WAL。 |
| `LoadSSTableFromDir`             | 加载目录中所有 SSTable | 启动时恢复                     | 扫描 `.sst` 文件，按数字排序后逐个打开并加入 `SSTableManager`。 |

---

## 7. `merging_iterator.h` —— 归并迭代器

| 函数/方法                                            | 目的               | 作用                                   | 实现思路                                                     |
| ---------------------------------------------------- | ------------------ | -------------------------------------- | ------------------------------------------------------------ |
| `MergingIterator::MergingIterator(iters)`            | 构造函数           | 初始化多路归并                         | 保存所有子迭代器，将所有有效子迭代器的当前键和索引压入最小堆。然后调用 `Next()` 定位到第一个有效键。 |
| `MergingIterator::Next()`                            | 移动到下一个有效键 | 处理相同键去重和墓碑跳过               | 弹出堆顶，记录当前键值；推进该迭代器，重新入堆；**跳过堆中所有相同键的其他迭代器**（推进并重新入堆）；若当前是墓碑则继续循环，否则设置 `valid_=true`。 |
| `SkipListIteratorWrapper` / `SSTableIteratorWrapper` | 适配器             | 将具体迭代器包装为 `IteratorInterface` | 存储具体迭代器对象，在虚函数中转发调用。                     |

---

## 8. `compaction.h` —— 合并辅助函数（遗留）

| 函数                                | 目的                             | 作用                      | 实现思路                                                     |
| ----------------------------------- | -------------------------------- | ------------------------- | ------------------------------------------------------------ |
| `ReadAllEntries(filename, entries)` | 读取整个 SSTable 的全部数据      | 用于合并时读取旧文件      | 读取 Footer 获取索引偏移，加载索引，然后遍历每个索引偏移，在数据区读取 flags、key、value，组装成 `(key, value, tombstone)`。 |
| `CompactSSTable`                    | 全量合并 SSTableManager 所有文件 | 将多个 SSTable 合并为一个 | 使用 `MergingIterator` 归并所有文件的有效数据，生成新 SSTable，删除旧文件，重新加载。 |
| `MergeAndScan`                      | 多源归并扫描                     | 从多个有序向量中扫描范围  | 使用优先队列进行多路归并，处理相同键时保留最新版本，跳过墓碑，在 `[start, end]` 范围内输出。 |

---

## 9. `leveled_compaction.h` —— 分层合并核心

| 函数/方法                                         | 目的                       | 作用                               | 实现思路                                                     |
| ------------------------------------------------- | -------------------------- | ---------------------------------- | ------------------------------------------------------------ |
| `Level::AddSSTable(sst)`                          | 向某层添加文件             | 更新总字节数，L1+ 触发整理         | 加入 `files_`，累计 `total_bytes_`，若层号>0则调用 `MaintainOrderAndNonOverlap()`。 |
| `Level::GetOverlappingFiles(start, end)`          | 获取与键范围重叠的文件索引 | 用于合并时选择目标层重叠文件       | L0 遍历所有文件；L1+ 二分查找第一个 `min_key >= start`，然后顺序收集直到 `min_key > end`。 |
| `Level::MaintainOrderAndNonOverlap`               | 维护 L1+ 文件有序且无重叠  | 合并重叠文件                       | 按最小键排序，扫描相邻文件，若重叠则读取所有文件内容，归并去重，生成新文件替换，递归整理。 |
| `LeveledCompaction::AddSSTable(sst)`              | 添加文件到 L0              | 触发持久化和后台合并               | 加锁，加入 L0，调用 `PersistManifest()`，调用 `SignalCompact()`。 |
| `LeveledCompaction::Get(key, value)`              | 点查询                     | 逐层查找                           | 加锁，从 L0 到最高层依次调用 `Level::Find`，找到即返回。     |
| `LeveledCompaction::DoCompact(level, file_index)` | 执行一次合并               | 将源层一个文件与目标层重叠文件合并 | 移出源文件，获取目标层重叠文件，备份所有文件路径；读取所有条目，归并去重；生成新 SSTable；关闭旧文件流，尝试删除；若成功则新文件加入目标层，持久化 Manifest；否则回滚。 |
| `LeveledCompaction::BackgroundCompactLoop`        | 后台线程主循环             | 异步执行合并                       | 等待 `need_compact_` 条件变量；遍历层级，若某层超限则执行 `DoCompact` 并持久化；重复直到不再超限。 |
| `LeveledCompaction::LoadFromDir`                  | 启动时恢复层级             | 从 Manifest 或扫描恢复             | 加锁，尝试 `Manifest::Load`；若成功则根据元数据重建各层；否则扫描所有 `.sst` 放入 L0，执行 `ForceCompaction`，最后 `PersistManifest`。 |
| `LeveledCompaction::PersistManifest`              | 持久化当前层级元数据       | 保存状态                           | 收集每一层的 `FileMeta`，调用 `Manifest::Save`。             |

---

## 10. `manifest.h` —— 元数据持久化

| 函数/方法                          | 目的                                 | 作用               | 实现思路                                                     |
| ---------------------------------- | ------------------------------------ | ------------------ | ------------------------------------------------------------ |
| `FileMeta::Serialize/Deserialize`  | 序列化/反序列化单个文件元数据        | 用于 Manifest 读写 | 依次读写 filename, file_size, first_key, last_key, version。 |
| `Manifest::Save(levels)`           | 保存所有层级元数据到新 MANIFEST 文件 | 原子更新           | 生成新序列号，写入魔数、版本、层数、每层的文件元数据；写入临时 CURRENT 文件，最后 rename 原子替换。 |
| `Manifest::Load(levels, last_seq)` | 从当前 MANIFEST 加载                 | 恢复层级           | 读取 CURRENT 获得最新序号，打开 `MANIFEST-<seq>`，校验魔数和版本，逐层读取 FileMeta 重建。 |

---

## 11. `main.cpp` —— 主程序

| 函数               | 目的              | 作用                      | 实现思路                                                     |
| ------------------ | ----------------- | ------------------------- | ------------------------------------------------------------ |
| `FlushToCompactor` | 将内存表刷写到 L0 | 持久化内存数据            | 收集内存表所有条目，构建 SSTable，调用 `g_compactor.AddSSTable`，清空内存表和 WAL。 |
| `AutoCheckpoint`   | 自动触发 Flush    | 根据内存表大小或 WAL 大小 | 检查自上次 Flush 后新增条目数或 `wal.log` 文件大小，超过阈值则调用 `FlushToCompactor`。 |
| `main` 命令行循环  | 处理用户输入      | 提供交互式界面            | 循环读取命令，调用相应功能（put/get/del/scan/begin/commit 等），并在写操作后调用 `AutoCheckpoint`。 |

---

