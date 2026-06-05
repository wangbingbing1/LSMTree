//
// Created by wangbingbing on 2026/5/23.
//

#ifndef TEST3_INC_COMPACTION_H_
#define TEST3_INC_COMPACTION_H_

#include <string>
#include <vector>
#include <memory>
#include <queue>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <tuple>

#include "sstable.h"
#include "merging_iterator.h"

namespace fs = std::filesystem;

/**
 * 从指定的 SSTable 文件中读取所有键值对（包括墓碑标记）到 entries 中。
 * @param filename   SSTable 文件路径
 * @param entries    输出参数，存储读取到的 (key, value, is_tombstone) 元组
 * @return           成功返回 true，否则 false
 */
template<typename K, typename V>
static bool ReadAllEntries(const std::string &filename,
                           std::vector<std::tuple<K, V, bool>> &entries) {
  std::ifstream in(filename, std::ios::binary);
  if (!in.is_open()) return false;

  // 获取文件总大小
  in.seekg(0, std::ios::end);
  uint64_t file_size = in.tellg();
  if (file_size < 8) return false;               // 文件至少需要包含 8 字节 Footer

  // 1. 读取 Footer（最后 8 字节）获得索引区起始偏移量
  in.seekg(-8, std::ios::end);
  uint64_t index_offset;
  ReadBinary(in, index_offset);

  if (file_size >= 16) {
    in.seekg(-16, std::ios::end);
    ReadBinary(in, index_offset);
    uint64_t bloom_offset;
    ReadBinary(in, bloom_offset);
  }

  // 2. 跳转到索引区，读取索引条目数
  in.seekg(index_offset, std::ios::beg);
  uint32_t index_size;
  ReadBinary(in, index_size);

  // 3. 读取所有索引条目，分别保存键和对应的数据区偏移量
  std::vector<uint64_t> offsets;
  std::vector<K> keys;
  for (uint32_t i = 0; i < index_size; ++i) {
    K key;
    ReadBinary(in, key);           // 从索引区读出键
    uint64_t offset;
    ReadBinary(in, offset);        // 读出该键对应的数据区偏移量（8 字节）
    keys.push_back(key);
    offsets.push_back(offset);
  }

  // 4. 根据每个偏移量跳转到数据区，读取该条记录的实际内容
  for (size_t i = 0; i < offsets.size(); ++i) {
    in.seekg(offsets[i], std::ios::beg);  // 定位到数据区记录的起始位置

    // 读取 1 字节墓碑标记
    bool tombstone = ReadFlags(in);

    // 读取键（此处读出仅用于跳过，因键已在索引中获取，未做一致性校验）
    K key;
    ReadBinary(in, key);

    // 读取值（仅当非墓碑时才有值数据）
    V value;
    if (!tombstone) {
      ReadBinary(in, value);
    }

    // 将完整记录添加到结果集
    entries.emplace_back(key, value, tombstone);
  }
  return true;
}

/**
 * 合并 SSTableManager 中的所有 SSTable 文件为一个新的 SSTable。
 * 利用 MergingIterator 多路归并，自动去重、跳过墓碑，仅保留有效键值对。
 *
 * @param sst_manager  SSTable 管理器，合并后内部状态被刷新（清空旧表，加载新表）
 * @param sst_dir      SSTable 文件存储目录
 */
template<typename K, typename V>
void CompactSSTable(SSTableManager<K, V> &sst_manager,
                    const std::string &sst_dir) {
  // 获取当前所有 SSTable 的引用（假设 SSTableManager 提供 GetTables() 返回 vector）
  const auto &tables = sst_manager.GetTables();
  if (tables.size() <= 1) return;   // 少于两个文件无需合并

  std::cout << "[Compaction] Starting compaction of " << tables.size() << " files...\n";

  // ----- 1. 构建归并迭代器（从新到旧，保证最新数据优先）-----
  std::vector<std::unique_ptr<IteratorInterface<K, V>>> iters;
  // 逆序遍历 tables：最新生成的 SSTable 索引最大（尾部），先加入迭代器列表
  for (auto it = tables.rbegin(); it != tables.rend(); ++it) {
    auto sst_iter = (*it)->NewIterator();
    if (sst_iter.Valid()) {
      // 将原生迭代器适配为统一的 IteratorInterface，并移入列表
      iters.push_back(std::make_unique<SSTableIteratorWrapper<K, V>>(std::move(sst_iter)));
    }
  }

  // MergingIterator 自动进行多路归并：最小堆、同键保留最新、跳过墓碑
  MergingIterator<K, V> merge_iter(std::move(iters));

  // ----- 2. 收集合并后的有效记录（全部为非墓碑）-----
  std::vector<std::tuple<K, V, bool>> output;
  while (merge_iter.Valid()) {
    // 由于 MergingIterator 已跳过所有墓碑，此处墓碑标记固定为 false
    output.emplace_back(merge_iter.Key(), merge_iter.Value(), false);
    merge_iter.Next();
  }

  // 若合并后没有任何有效记录，则直接退出（不生成空文件）
  if (output.empty()) {
    std::cout << "[Compaction] No valid entries after merge, skipping.\n";
    return;
  }

  // ----- 3. 生成新的 SSTable 文件 -----
  int new_num = GetNextSSTableNumber(sst_dir);
  std::string new_filename = sst_dir + "/" + std::to_string(new_num) + ".sst";
  if (!SSTable<K, V>::Build(new_filename, output)) {
    std::cerr << "[Compaction] Failed to build new SSTable: " << new_filename << "\n";
    return;
  }

  // ----- 4. 删除所有旧的 SSTable 文件 -----
  for (const auto &sst : tables) {
    std::error_code ec;
    fs::remove(sst->Filename(), ec);
    if (ec) {
      std::cerr << "[Compaction] Failed to remove " << sst->Filename() << ": "
                << ec.message() << "\n";
    }
  }

  // ----- 5. 刷新管理器并重新加载（只包含新生成的文件）-----
  sst_manager.Clear();
  LoadSSTableFromDir(sst_dir, sst_manager);

  std::cout << "[Compaction] Completed, new file: " << new_filename
            << ", entries: " << output.size() << "\n";
}

/**
 * @tparam K        键类型
 * @tparam V        值类型
 * @param sources   多个有序数据源（每个源必须按键升序）
 * @param start     范围起始键（包含）
 * @param end       范围结束键（包含）
 * @param output    输出回调，接受 (key, value)
 */
template<typename K, typename V>
void MergeAndScan(
    const std::vector<const std::vector<std::tuple<K, V, bool>>*> &sources,
    const K &start,
    const K &end,
    std::function<void(const K &, const V &)> output) {

  // ptrs[i] 记录第 i 个源当前待处理的元素下标
  std::vector<size_t> ptrs(sources.size(), 0);

  // 比较器：决定堆中哪个元素优先弹出（实现最小堆）
  auto cmp = [&](size_t i, size_t j) {
    const auto &ei = (*sources[i])[ptrs[i]];
    const auto &ej = (*sources[j])[ptrs[j]];
    const K &key_i = std::get<0>(ei);
    const K &key_j = std::get<0>(ej);
    // 键小的优先
    if (key_i != key_j) return key_i > key_j;
    // 键相同时，索引大的（更新）优先弹出
    return i < j;   // i < j 时 i 优先级低，因此 j 更优先
  };

  // 优先队列存储源索引，按照 cmp 规则排序
  std::priority_queue<size_t, std::vector<size_t>, decltype(cmp)> pq(cmp);

  // 初始化：将每个源中第一个 >= start 的元素入堆（跳过所有小于 start 的）
  for (size_t i = 0; i < sources.size(); ++i) {
    auto &vec = *sources[i];
    size_t &p = ptrs[i];
    // 跳过所有 key < start 的记录
    while (p < vec.size() && std::get<0>(vec[p]) < start)
      ++p;
    // 如果还有剩余记录，且 key <= end（可在此处过滤，提高效率）
    if (p < vec.size() && std::get<0>(vec[p]) <= end)
      pq.push(i);
  }

  K current_key;
  V current_value;
  bool current_tomb = false;
  bool has_current = false;   // 是否暂存了当前正在处理的键的最新版本

  while (!pq.empty()) {
    size_t idx = pq.top();
    pq.pop();
    auto &vec = *sources[idx];
    size_t &p = ptrs[idx];
    const auto &entry = vec[p];
    K key = std::get<0>(entry);
    V value = std::get<1>(entry);
    bool tomb = std::get<2>(entry);

    // 如果当前弹出的键已经超出范围，由于所有源有序，后续键都会更大，直接结束
    if (key > end) break;

    // 推进该源的指针，并决定是否重新入堆（下一个键必须在范围内）
    ++p;
    if (p < vec.size() && std::get<0>(vec[p]) <= end) {
      pq.push(idx);
    }

    // 合并逻辑：处理同键的多个版本
    if (!has_current) {
      // 第一个遇到的键，直接暂存
      current_key = key;
      current_value = value;
      current_tomb = tomb;
      has_current = true;
    } else if (key == current_key) {
      // 相同键：用新版本覆盖（旧版本被丢弃）
      current_value = value;      // current_key = value
      current_tomb = tomb;
    } else {
      // 新键：先输出上一个键（如果非墓碑且未超出范围）
      if (!current_tomb && current_key <= end) {
        output(current_key, current_value);
      }
      // 暂存新键
      current_key = key;
      current_value = value;
      current_tomb = tomb;
    }
  }

  // 输出最后一个暂存的键（如果有效）
  if (has_current && !current_tomb && current_key <= end) {
    output(current_key, current_value);
  }
}

#endif //TEST3_INC_COMPACTION_H_