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
 * 实现方式：多路归并（最小堆），每个键的旧版本在弹出后立即丢弃。
 *
 * @param sst_manager  SSTable 管理器，合并后内部状态被刷新（清空旧表，加载新表）
 * @param sst_dir      SSTable 文件存储目录
 */
template<typename K, typename V>
void CompactSSTable(SSTableManager<K, V> &sst_manager,
                    const std::string &sst_dir) {
  // 获取当前所有 SSTable 的引用（假设 SSTableManager 提供 GetALL() 返回 vector）
  const auto &tables = sst_manager.GetTables();
  if (tables.size() <= 1) return;   // 少于两个文件无需合并

  std::cout << "[Compaction] Starting compaction of " << tables.size() << " files...\n";

  // ---------- 1. 读取所有 SSTable 的全部记录 ----------
  // all_entries[i] 保存第 i 个文件的全部记录 (key, value, is_tombstone)
  // 文件索引 i 越大，文件越新（因为加载顺序由旧到新，下标递增代表新旧顺序）
  std::vector<std::vector<std::tuple<K, V, bool>>> all_entries;
  all_entries.reserve(tables.size());
  for (size_t i = 0; i < tables.size(); ++i) {
    std::vector<std::tuple<K, V, bool>> entries;
    // ReadAllEntries 从 SSTable 文件中读出所有记录
    if (!ReadAllEntries<K, V>(tables[i]->Filename(), entries)) {
      std::cerr << "[Compaction] Failed to read " << tables[i]->Filename() << ", abort.\n";
      return;
    }
    all_entries.push_back(std::move(entries));
  }

  // ---------- 2. 多路归并（最小堆） ----------
  // ptrs[i] 表示第 i 个文件当前待处理的记录下标
  std::vector<size_t> ptrs(all_entries.size(), 0);

  // 用于构建最小堆
  auto cmp = [&](size_t i, size_t j) {
    const auto& ei = all_entries[i][ptrs[i]];
    const auto& ej = all_entries[j][ptrs[j]];
    // 先按 key 升序：key 更小的优先
    if (std::get<0>(ei) != std::get<0>(ej))
      return std::get<0>(ei) > std::get<0>(ej);
    // key 相同时，文件索引大的优先弹出，索引小的优先级低
    return i < j;
  };
  std::priority_queue<size_t, std::vector<size_t>, decltype(cmp)> pq(cmp);

  // 初始化堆：将每个文件的第一条记录加入堆
  for (size_t i = 0; i < all_entries.size(); ++i) {
    if (ptrs[i] < all_entries[i].size())
      pq.push(i);
  }

  std::vector<std::tuple<K, V, bool>> output;   // 最终写入新 SSTable 的记录

  while (!pq.empty()) {
    // 弹出堆顶：当前所有文件中键最小且版本最新的一条记录
    size_t idx = pq.top();
    pq.pop();
    const auto& entry = all_entries[idx][ptrs[idx]];
    K key = std::get<0>(entry);
    bool first_is_tombstone = std::get<2>(entry);   // 最新版本是否为墓碑
    V first_value = std::get<1>(entry);             // 最新版本的值

    // 推进该文件的指针，如果还有数据则重新入堆
    ptrs[idx]++;
    if (ptrs[idx] < all_entries[idx].size())
      pq.push(idx);

    // 跳过所有与该键相同的旧版本记录（它们在堆中连续排列）
    while (!pq.empty()) {
      size_t next_idx = pq.top();
      const auto& next_entry = all_entries[next_idx][ptrs[next_idx]];
      if (std::get<0>(next_entry) != key)
        break;   // 遇到不同的键，停止
      // 相同键的旧版本：弹出并推进指针，直接丢弃
      pq.pop();
      ptrs[next_idx]++;
      if (ptrs[next_idx] < all_entries[next_idx].size())
        pq.push(next_idx);
    }

    // 根据最新版本决定是否保留该键
    if (first_is_tombstone)
      continue;   // 最新版本是墓碑，键已被删除，不加入输出

    // 最新版本是有效值，保留
    output.emplace_back(key, first_value, false);
  }

  // ---------- 3. 生成新的 SSTable 文件 ----------
  int new_num = GetNextSSTableNumber(sst_dir);
  std::string new_filename = sst_dir + "/" + std::to_string(new_num) + ".sst";
  if (!SSTable<K, V>::Build(new_filename, output)) {
    std::cerr << "[Compaction] Failed to build new SSTable: " << new_filename << "\n";
    return;
  }

  // ---------- 4. 删除旧 SSTable 文件 ----------
  for (const auto& sst : tables) {
    std::error_code ec;
    fs::remove(sst->Filename(), ec);
    if (ec) std::cerr << "[Compaction] Failed to remove " << sst->Filename() << "\n";
  }

  // ---------- 5. 刷新管理器 ----------
  sst_manager.Clear();                        // 清空管理器中的旧 SSTable 对象
  LoadSSTableFromDir(sst_dir, sst_manager);  // 重新加载目录中的 SSTable（此时只有新文件）

  std::cout << "[Compaction] Completed, new file: " << new_filename
            << ", entries: " << output.size() << "\n";
}

#endif //TEST3_INC_COMPACTION_H_