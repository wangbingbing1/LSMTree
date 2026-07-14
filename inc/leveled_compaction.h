//
// Created by wangbingbing on 2026/5/27.
//

#ifndef CVSTORE_INC_LEVELED_COMPACTION_H_
#define CVSTORE_INC_LEVELED_COMPACTION_H_

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <thread>
#include <chrono>
#include <atomic>
#include <condition_variable>

#include "sstable.h"
#include "merging_iterator.h"
#include "compaction.h"   // 提供 ReadAllEntries, GetNextSSTableNumber
#include "manifest.h"

namespace fs = std::filesystem;

namespace leveled {

// ------------------辅助函数-------------------
inline bool RemoveFileWithRetry(const std::string &path, int max_retries = 3) {
  for (int i = 0; i < max_retries; ++i) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (!ec) return true;
    if (i == max_retries - 1) {
      std::cerr << "Failed to remove " << path << " after " << max_retries
                << " attempts: " << ec.message() << std::endl;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// 前向声明
template<typename K, typename V>
class Level;

// ========== 分层合并管理器 ==========
// 实现类似 LevelDB 的分层 Compaction 策略
template<typename K, typename V>
class LeveledCompaction {
 public:
  /**
   * 构造函数：初始化分层合并管理器
   * @param sst_dir      SSTable 文件存储目录
   * @param max_level    最大层数（0 ~ max_level-1），至少 2
   * @param level_sizes  每层容量上限（字节），长度必须等于 max_level
   *                     L0 通常设为较小值（如 8MB），L1 稍大，依次递增
   */
  LeveledCompaction(const std::string &sst_dir,
                    int max_level,
                    const std::vector<uint64_t> &level_sizes);

  ~LeveledCompaction();

  // 禁止拷贝，允许移动
  LeveledCompaction(const LeveledCompaction &) = delete;
  LeveledCompaction &operator=(const LeveledCompaction &) = delete;
  LeveledCompaction(LeveledCompaction &&) = default;
  LeveledCompaction &operator=(LeveledCompaction &&) = default;

  /**
   * 添加一个新生成的 SSTable（通常来自 MemTable Flush），总是加入 Level 0
   * @param sst 待添加的 SSTable 指针（所有权转移）
   */
  void AddSSTable(std::unique_ptr<SSTable<K, V>> sst);

  /**
   * 点查询：从 Level 0 到 Level N-1 依次查找，找到即返回
   * @param key   要查找的键
   * @param value [out] 找到的值
   * @return      键存在且非墓碑时返回 true
   */
  bool Get(const K &key, V &value) const;

  /**
   * 触发合并检查（通常在 AddSSTable 或 Flush 后调用）
   * 检查各层是否超过容量限制，若有则触发合并
   */
  void MaybeCompact();

  /**
   * 启动时从目录加载所有 SSTable 文件（按文件名约定恢复分层状态）
   * 扫描目录中所有 .sst 文件，全部放入 L0，然后强制整理
   */
  void LoadFromDir();

  /**
   * 打印各层统计信息（用于调试）
   */
  void PrintStats() const;

  /**
   * 强制合并所有 L0 文件到下一层（用于 save/flush 命令后彻底整理）
   */
  void ForceCompaction();

  /**
   * 获取所有 SSTable 的迭代器（按文件生成时间倒序，新文件在前）
   * 用于全量扫描（如 range query）
   * @return 迭代器列表
   */
  std::vector<std::unique_ptr<IteratorInterface<K, V>>> GetAllIterators() const;

 private:
  std::string sst_dir_;                       // SSTable 文件目录
  int max_level_;                             // 最大层数
  std::vector<uint64_t> level_sizes_;         // 各层容量上限（字节）
  std::vector<std::unique_ptr<Level<K, V>>> levels_; // 各层对象
  mutable std::mutex mu_;                     // 线程安全锁
  std::thread bg_thread_;                     // 后台 Compaction 工作线程，异步执行合并操作
  std::condition_variable cv_;                // 条件变量，用于在合并任务到来或线程停止时唤醒后台线程
  std::mutex compact_mu_;                     // 互斥锁，保护 need_compact_、stop_ 等合并相关状态
  bool stop_ = false;                         // 停止标志：为 true 时后台线程应退出运行
  bool need_compact_ = false;                 // 合并请求标志：为 true 表示存在待处理的合并任务

  /**
   * 返回下一层编号，若已是最高层则返回 -1
   */
  int NextLevel(int level) const {
    return (level + 1 < max_level_) ? level + 1 : -1;
  }

  /**
   * 执行一次合并：将 level 层的某个文件（file_index）合并到下一层
   * @param level      源层号
   * @param file_index 源层中待合并文件的索引
   */
  void DoCompact(int level, size_t file_index);

  /**
   * 从 L0 中选择一个文件进行合并（策略：选择与下层重叠文件最多的文件）
   * 这样能最大程度减少后续合并的写放大
   * @return 选中的文件索引
   */
  size_t PickFileToCompactFromL0() const;

/**
 * 唤醒后台合并线程：设置 need_compact_ 标志并通知条件变量。
 */
  void SignalCompact() {
    {
      std::lock_guard<std::mutex> lk(compact_mu_);
      need_compact_ = true;   // 标记有合并任务待处理
    }
    cv_.notify_one();           // 唤醒一个正在等待的后台线程
  }

/**
 * 后台 Compaction 工作循环。
 * 作为独立线程运行，等待 need_compact_ 信号后执行分级合并，直到 stop_ 为 true。
 */
  void BackgroundCompactLoop();

/**
 * 将当前所有层级的元数据保存到 MANIFEST 文件。
 * @return 成功返回 true，失败返回 false
 */
  bool PersistManifest();
};

// ========== 单个层 ==========
// 表示 LSM 树的某一层，管理属于该层的所有 SSTable
template<typename K, typename V>
class Level {
 public:
  /**
   * 构造函数
   * @param level_num  层号（0 为 L0，允许重叠；≥1 为 L1+，要求有序且无重叠）
   * @param size_limit 该层的容量上限（字节），超过会触发 Compaction
   */
  explicit Level(int level_num, uint64_t size_limit);

  /**
   * 向该层添加一个 SSTable 文件
   * @param sst 待添加的 SSTable 指针（所有权转移）
   */
  void AddSSTable(std::unique_ptr<SSTable<K, V>> sst);

  /**
   * 移除指定索引的文件，返回被移除的指针（从该层中删除）
   * @param index 文件索引
   * @return      被移除的 SSTable 指针
   */
  std::unique_ptr<SSTable<K, V>> RemoveFile(size_t index);

  /**
   * 在该层内查找键 key，若找到则通过 value 返回并返回 true
   * @param key   要查找的键
   * @param value [out] 找到的值
   * @return      找到且非墓碑时返回 true
   */
  bool Find(const K &key, V &value) const;

  /**
   * 获取所有文件的只读引用
   */
  const std::vector<std::unique_ptr<SSTable<K, V>>> &GetFiles() const { return files_; }

  /**
   * 返回与键范围 [start, end] 重叠的文件索引列表
   * @param start 范围起始键（包含）
   * @param end   范围结束键（包含）
   * @return      重叠文件的索引列表
   */
  std::vector<size_t> GetOverlappingFiles(const K &start, const K &end) const;

  /**
   * 返回该层当前总字节数
   */
  uint64_t TotalBytes() const { return total_bytes_; }

  /**
   * 判断该层是否超过容量限制
   */
  bool ExceedsLimit() const { return total_bytes_ > size_limit_; }

  /**
   * 返回层号
   */
  int LevelNum() const { return level_num_; }

  /**
   * 清空该层的所有文件（删除内存指针，不删除磁盘文件）
   */
  void Clear();

  /**
   * 收集当前层中所有 SSTable 文件的元数据信息（FileMeta），用于保存到 Manifest。
   * @param metas  输出参数，清空后填充每个文件的元数据
   */
  void GetAllFileMeta(std::vector<FileMeta<K>> &metas) const;

 private:
  int level_num_;                             // 当前层层号（0 为 L0，允许重叠；≥1 为 L1+，要求有序且无重叠）
  uint64_t size_limit_;                       // 该层的容量上限（字节），超过会触发 Compaction
  uint64_t total_bytes_;                      // 该层当前所有 SSTable 文件的总大小（字节）
  std::vector<std::unique_ptr<SSTable<K, V>>> files_; // 存储该层所有 SSTable 文件的独占指针，L0 可能无序，L1+ 保持按键升序且无重叠

  /**
   * 维护 L1+ 层文件的有序性与非重叠性：
   * - 先按键的最小键排序
   * - 检测并合并所有键范围重叠的相邻文件
   * - 合并时按版本降序保留每个键的最新非墓碑值
   * 仅当 level_num_ > 0 时调用，L0 无需此约束。
   */
  void MaintainOrderAndNonOverlap();
};

// ==================== Level 实现 ====================

// 构造函数：初始化层号、容量上限，总字节数为0
template<typename K, typename V>
Level<K, V>::Level(int level_num, uint64_t size_limit)
    : level_num_(level_num), size_limit_(size_limit), total_bytes_(0) {}

// 向该层添加一个 SSTable 文件
template<typename K, typename V>
void Level<K, V>::AddSSTable(std::unique_ptr<SSTable<K, V>> sst) {
  if (!sst) return;
  total_bytes_ += sst->FileSize();       // 累计该层总大小
  files_.push_back(std::move(sst));      // 接管文件所有权
  // L1 及以上层必须维护文件有序且无重叠
  if (level_num_ > 0) {
    MaintainOrderAndNonOverlap();
  }
}

// 移除指定索引的文件，返回被移除的指针（从该层中删除）
template<typename K, typename V>
std::unique_ptr<SSTable<K, V>> Level<K, V>::RemoveFile(size_t index) {
  if (index >= files_.size()) return nullptr;
  auto removed = std::move(files_[index]);
  removed->CloseFile();
  total_bytes_ -= removed->FileSize();   // 更新总字节数
  files_.erase(files_.begin() + index);  // 从 vector 中删除
  return removed;
}

// 在该层中查找键 key，若找到则通过 value 返回并返回 true
template<typename K, typename V>
bool Level<K, V>::Find(const K &key, V &value) const {
  if (level_num_ == 0) {
    // L0：文件可能重叠，必须从最新（最后添加）到最旧遍历
    for (auto it = files_.rbegin(); it != files_.rend(); ++it) {
      if ((*it)->Get(key, value)) return true;
    }
    return false;
  }

  // L1+：文件有序且不重叠，可使用二分查找
  int lo = 0, hi = static_cast<int>(files_.size()) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    const auto &sst = files_[mid];
    K first, last;
    sst->GetFirstKey(first);
    sst->GetLastKey(last);
    if (key < first) {
      hi = mid - 1;          // 目标键在左侧
    } else if (key > last) {
      lo = mid + 1;          // 目标键在右侧
    } else {
      // 键落在该文件范围内，具体查询交给文件自身
      return sst->Get(key, value);
    }
  }
  return false;
}

// 返回该层中与键范围 [start, end] 有重叠的文件索引列表
template<typename K, typename V>
std::vector<size_t> Level<K, V>::GetOverlappingFiles(const K &start, const K &end) const {
  std::vector<size_t> result;
  if (level_num_ == 0) {
    // L0：所有文件都可能重叠，需逐个检查
    for (size_t i = 0; i < files_.size(); ++i) {
      K first, last;
      files_[i]->GetFirstKey(first);
      files_[i]->GetLastKey(last);
      // 判断区间是否重叠：不满足 (last < start || end < first) 即重叠
      if (!(last < start || end < first))
        result.push_back(i);
    }
    return result;
  }

  // L1+：文件有序无重叠，二分查找第一个可能的文件，然后顺序收集直到超出范围
  int lo = 0, hi = static_cast<int>(files_.size());
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    K first;
    files_[mid]->GetFirstKey(first);
    if (first < start) {
      lo = mid + 1;   // 该文件的最小键在 start 左边，向右搜索
    } else {
      hi = mid;
    }
  }
  // lo 是第一个 min_key >= start 的文件索引（或越界）
  for (size_t i = lo; i < files_.size(); ++i) {
    K first;
    files_[i]->GetFirstKey(first);
    if (first > end) break;     // 超出范围，后续不可能有重叠
    result.push_back(i);
  }
  return result;
}

// 清空该层的所有文件（只清空内存，不删除磁盘）
template<typename K, typename V>
void Level<K, V>::Clear() {
  files_.clear();
  total_bytes_ = 0;
}

// 维护 L1+ 层文件的有序且无重叠（L0 不调用此函数）
template<typename K, typename V>
void Level<K, V>::MaintainOrderAndNonOverlap() {
  if (files_.size() <= 1) return;

  // 1. 按键的最小键排序
  std::sort(files_.begin(), files_.end(),
            [](const auto &a, const auto &b) {
              K ka, kb;
              a->GetFirstKey(ka);
              b->GetFirstKey(kb);
              return ka < kb;
            });

  // 辅助：从文件名中提取版本号（数字.sst）
  auto get_version = [](const std::string &fname) -> uint64_t {
    std::string stem = fs::path(fname).stem().string();
    return std::stoull(stem);
  };

  // 扫描相邻文件，检测并合并重叠区域
  for (size_t i = 1; i < files_.size();) {
    size_t start = i - 1;                 // 重叠区间的起始索引
    size_t end = i;                       // 当前扫描位置（第一个不重叠的索引）
    K cur_last;
    files_[start]->GetLastKey(cur_last);  // 重叠区间的最大键

    // 扩展重叠区间：end 不断后移，直到遇到第一个不重叠的文件
    while (end < files_.size()) {
      K next_first;
      files_[end]->GetFirstKey(next_first);
      if (next_first <= cur_last) {      // 存在重叠
        K this_last;
        files_[end]->GetLastKey(this_last);
        // 关键：取最大值以覆盖重叠区域的最大上界，避免遗漏后续重叠文件
        if (this_last > cur_last) cur_last = this_last;
        ++end;
      } else {
        break;
      }
    }

    // 如果区间没有扩展（即当前这对文件不重叠），继续检查下一对
    if (end == i) {
      ++i;
      continue;
    }

    // --- 存在重叠区间 [start, end)，需要合并 ---
    std::cerr << "Warning: overlapping SSTables in level " << level_num_
              << " (" << (end - start) << " files), merging.\n";

    // 保存旧文件路径和原始指针（用于回滚）
    std::vector<std::string> old_paths;
    std::vector<std::unique_ptr<SSTable<K, V>>> old_ssts;
    for (size_t idx = start; idx < end; ++idx) {
      old_paths.push_back(files_[idx]->Filename());
      old_ssts.push_back(std::move(files_[idx]));
    }
    // 从 files_ 中删除这些文件
    files_.erase(files_.begin() + start, files_.begin() + end);

    // 收集所有文件的数据及其版本号（此时 old_ssts 中对象仍存在，可以读取）
    std::vector<std::pair<uint64_t, std::vector<std::tuple<K, V, bool>>>> file_data;
    bool read_failed = false;
    for (size_t idx = 0; idx < old_ssts.size(); ++idx) {
      uint64_t ver = get_version(old_ssts[idx]->Filename());
      std::vector<std::tuple<K, V, bool>> entries;
      if (!ReadAllEntries(old_ssts[idx]->Filename(), entries)) {
        std::cerr << "Failed to read " << old_ssts[idx]->Filename() << "\n";
        read_failed = true;
        break;
      }
      file_data.emplace_back(ver, std::move(entries));
    }
    if (read_failed) {
      // 读取失败，无法合并，将旧文件放回并继续
      for (auto &sst : old_ssts) {
        files_.push_back(std::move(sst));
      }
      // 重新排序
      MaintainOrderAndNonOverlap();
      return;
    }

    // 按版本号降序（新文件在前），确保最新数据优先处理
    std::sort(file_data.begin(), file_data.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    // 归并：每个键只保留第一次遇到（最新版本）且非墓碑的条目
    std::vector<std::tuple<K, V, bool>> merged;
    std::unordered_set<K> seen_keys;
    for (const auto &[ver, entries] : file_data) {
      for (const auto &entry : entries) {
        const K &key = std::get<0>(entry);
        bool tombstone = std::get<2>(entry);
        if (seen_keys.find(key) != seen_keys.end())
          continue;
        if (!tombstone) {
          merged.push_back(entry);
        }
        seen_keys.insert(key);
      }
    }

    // 生成新文件名
    std::string dir = fs::path(old_paths[0]).parent_path().string();
    int new_seq = GetNextSSTableNumber(dir);
    std::string new_filename = dir + "/" + std::to_string(new_seq) + ".sst";

    // 如果没有有效数据或构建失败，则恢复旧文件并退出
    if (merged.empty() || !SSTable<K, V>::Build(new_filename, merged)) {
      if (!merged.empty()) {
        std::cerr << "Failed to build merged SSTable\n";
      }
      // 恢复旧文件
      for (auto &sst : old_ssts) {
        files_.push_back(std::move(sst));
      }
      ++i;
      continue;
    }

    // ---------- 合并成功，准备替换 ----------
    // 先关闭旧文件的所有文件流，以便安全删除
    for (auto &sst : old_ssts) {
      sst->CloseFile();   // 假设 SSTable 有 CloseFile() 方法
    }
    // 释放旧文件对象（不再需要）
    old_ssts.clear();

    // 尝试删除所有旧物理文件
    bool all_deleted = true;
    for (const auto &path : old_paths) {
      if (!RemoveFileWithRetry(path)) {
        all_deleted = false;
      }
    }

    if (!all_deleted) {
      // 删除失败，回滚：删除新生成的文件，从磁盘重新加载旧文件到内存
      std::error_code ec;
      fs::remove(new_filename, ec);   // 删除刚生成的新文件
      // 重新加载旧文件（它们还在磁盘上）
      for (const auto &path : old_paths) {
        auto reloaded = std::make_unique<SSTable<K, V>>(path);
        files_.push_back(std::move(reloaded));
      }
      // 重新计算 total_bytes_ 并排序
      total_bytes_ = 0;
      for (auto &f : files_) total_bytes_ += f->FileSize();
      MaintainOrderAndNonOverlap();   // 重新整理（可能还会触发合并）
      std::cerr << "Compaction rolled back due to file deletion failure.\n";
      return;
    }

    // 删除成功，创建新 SSTable 并加入
    auto new_sst = std::make_unique<SSTable<K, V>>(new_filename);
    uint64_t new_size = new_sst->FileSize();

    // 更新总字节数：减去旧文件大小（我们已经从 files_ 中删除了它们，所以重新计算）
    total_bytes_ = 0;
    for (auto &f : files_) total_bytes_ += f->FileSize();
    total_bytes_ += new_size;

    // 将新文件插入到起始位置（保持有序）
    files_.insert(files_.begin() + start, std::move(new_sst));

    // 合并后可能产生新的重叠，递归重新检查
    MaintainOrderAndNonOverlap();
    return;
  }
}

//
template<typename K, typename V>
void Level<K, V>::GetAllFileMeta(std::vector<FileMeta<K>> &metas) const {
metas.clear();
metas.reserve(files_.size());                     // 预分配空间，避免多次扩容

for (const auto &sst : files_) {
FileMeta<K> meta;

// 仅保存基础文件名（不含路径），恢复时会在目录下重建
meta.filename = fs::path(sst->Filename()).filename().string();

// 文件当前占用磁盘大小
meta.file_size = sst->FileSize();

// 从 SSTable 中读取最小键和最大键
sst->GetFirstKey(meta.first_key);
sst->GetLastKey(meta.last_key);

// 版本号从文件名主干的数字部分解析（如 "123.sst" -> 123）
std::string stem = fs::path(sst->Filename()).stem().string();
meta.version = std::stoull(stem);

metas.push_back(std::move(meta));
}
}

// ==================== LeveledCompaction 实现 ====================

// 构造函数：初始化 SSTable 目录、最大层级数、各层容量限制
template<typename K, typename V>
LeveledCompaction<K, V>::LeveledCompaction(const std::string &sst_dir,
                                           int max_level,
                                           const std::vector<uint64_t> &level_sizes)
    : sst_dir_(sst_dir),
      max_level_(max_level),
      level_sizes_(level_sizes) {
  if (max_level_ < 2) max_level_ = 2;     // 至少两层（L0和L1）
  if (level_sizes_.size() < static_cast<size_t>(max_level_)) {
    level_sizes_.resize(max_level_, 64 * 1024 * 1024);
  }
  levels_.reserve(max_level_);
  // 初始化每一层
  for (int i = 0; i < max_level_; ++i) {
    levels_.push_back(std::make_unique<Level<K, V>>(i, level_sizes_[i]));
  }
  // 后台线程启动
  bg_thread_ = std::thread(&LeveledCompaction::BackgroundCompactLoop, this);
}

template<typename K, typename V>
LeveledCompaction<K, V>::~LeveledCompaction() {
  {
    std::lock_guard<std::mutex> lk(compact_mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (bg_thread_.joinable())
    bg_thread_.join();
}

// 添加一个新的 SSTable 文件（通常来自 Flush），放入 L0，并触发必要的合并
template<typename K, typename V>
void LeveledCompaction<K, V>::AddSSTable(std::unique_ptr<SSTable<K, V>> sst) {
  if (!sst) return;
  std::lock_guard<std::mutex> lock(mu_);    // 保证线程安全
  levels_[0]->AddSSTable(std::move(sst));      // 放入第0层
  PersistManifest();                           // 立即持久化
  SignalCompact();                             // 唤醒后台线程检查合并
}

// 在整个层级中查找键 key，从 L0 到高层依次查找
template<typename K, typename V>
bool LeveledCompaction<K, V>::Get(const K &key, V &value) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (int i = 0; i < max_level_; ++i) {
    if (levels_[i]->Find(key, value))
      return true;
  }
  return false;
}

// 检查各层是否超过容量限制，若有则触发合并
template<typename K, typename V>
void LeveledCompaction<K, V>::MaybeCompact() {
  SignalCompact();
}

// 从 L0 中选择一个文件进行合并（策略：选择与下一层重叠数最多的文件，以减少后续重叠）
template<typename K, typename V>
size_t LeveledCompaction<K, V>::PickFileToCompactFromL0() const {
  const auto &files = levels_[0]->GetFiles();
  if (files.empty()) return 0;

  // 统计每个文件与 L1 的重叠文件数
  std::vector<size_t> overlap_counts(files.size(), 0);
  for (size_t i = 0; i < files.size(); ++i) {
    K first, last;
    files[i]->GetFirstKey(first);
    files[i]->GetLastKey(last);
    // 注意：L0 的所有文件与 L1 的重叠情况通过 levels_[1] 查询
    overlap_counts[i] = levels_[1]->GetOverlappingFiles(first, last).size();
  }

  // 选择重叠最多的文件进行合并（以减少 L1 中涉及的合并文件数）
  size_t best_idx = 0;
  for (size_t i = 1; i < files.size(); ++i) {
    if (overlap_counts[i] > overlap_counts[best_idx])
      best_idx = i;
  }
  return best_idx;
}

template<typename K, typename V>
void LeveledCompaction<K, V>::DoCompact(int level, size_t file_index) {
  int next = NextLevel(level);
  if (next == -1) {
    std::cerr << "Level " << level << " is max, cannot compact further.\n";
    return;
  }

  auto &src_level = levels_[level];
  auto &dst_level = levels_[next];

  // 取出源文件
  auto src_file = src_level->RemoveFile(file_index);
  if (!src_file) return;

  K first, last;
  src_file->GetFirstKey(first);
  src_file->GetLastKey(last);

  // 找出目标层中与源文件重叠的所有文件，并移除
  auto overlap_indices = dst_level->GetOverlappingFiles(first, last);
  std::vector<std::unique_ptr<SSTable<K, V>>> overlapping;
  for (auto idx : overlap_indices) {
    overlapping.push_back(dst_level->RemoveFile(idx));
  }

  // ---------- 备份所有参与合并的文件 ----------
  std::vector<std::unique_ptr<SSTable<K, V>>> backup;
  backup.push_back(std::move(src_file));
  for (auto &f : overlapping) {
    backup.push_back(std::move(f));
  }
  overlapping.clear();  // 不再需要原始的 overlapping

  // 收集所有文件信息（版本号 + 路径）
  std::vector<std::pair<uint64_t, std::string>> files_to_merge;
  for (auto &sst : backup) {
    uint64_t seq = std::stoull(fs::path(sst->Filename()).stem().string());
    files_to_merge.emplace_back(seq, sst->Filename());
  }

  // 按版本降序排序
  std::sort(files_to_merge.begin(), files_to_merge.end(),
            [](const auto &a, const auto &b) { return a.first > b.first; });

  // 读取所有数据到内存（此时 backup 中的对象还持有文件，但我们需要读数据）
  std::vector<std::tuple<K, V, bool>> merged;
  std::unordered_set<K> seen_keys;
  for (const auto &[seq, fname] : files_to_merge) {
    std::vector<std::tuple<K, V, bool>> entries;
    if (!ReadAllEntries(fname, entries)) {
      std::cerr << "Failed to read " << fname << "\n";
      // 读取失败，回滚：将 backup 中的所有文件放回原层
      src_level->AddSSTable(std::move(backup[0]));  // 第一个是 src_file
      for (size_t i = 1; i < backup.size(); ++i) {
        dst_level->AddSSTable(std::move(backup[i]));
      }
      return;
    }
    for (auto &entry : entries) {
      const K &key = std::get<0>(entry);
      bool tombstone = std::get<2>(entry);
      if (seen_keys.find(key) != seen_keys.end()) continue;
      if (!tombstone) {
        merged.push_back(std::move(entry));
      }
      seen_keys.insert(key);
    }
  }

  // 如果没有有效条目，直接删除所有参与文件（不需要生成新文件）
  if (merged.empty()) {
    // 先关闭所有文件流以便删除
    for (auto &sst : backup) sst->CloseFile();
    backup.clear();
    for (const auto &[seq, fname] : files_to_merge) {
      RemoveFileWithRetry(fname);
    }
    return;
  }

  // 生成新 SSTable 文件名
  int new_seq = GetNextSSTableNumber(sst_dir_);
  std::string new_filename = sst_dir_ + "/" + std::to_string(new_seq) + ".sst";

  // 构建新 SSTable
  if (!SSTable<K, V>::Build(new_filename, merged)) {
    std::cerr << "Failed to build new SSTable during compaction: " << new_filename << "\n";
    // 回滚：将 backup 中的文件放回原层
    src_level->AddSSTable(std::move(backup[0]));
    for (size_t i = 1; i < backup.size(); ++i) {
      dst_level->AddSSTable(std::move(backup[i]));
    }
    return;
  }

  // ---------- 新文件构建成功，尝试删除旧文件 ----------
  // 先关闭所有旧文件的文件流
  for (auto &sst : backup) sst->CloseFile();
  backup.clear();   // 释放对象，文件流已关闭

  bool all_deleted = true;
  for (const auto &[seq, fname] : files_to_merge) {
    if (!RemoveFileWithRetry(fname)) {
      all_deleted = false;
    }
  }

  if (!all_deleted) {
    // 删除失败，回滚：删除新文件，重新加载旧文件到目标层
    std::error_code ec;
    fs::remove(new_filename, ec);
    // 重新加载旧文件（从磁盘）
    for (const auto &[seq, fname] : files_to_merge) {
      auto reloaded = std::make_unique<SSTable<K, V>>(fname);
      if (fname == files_to_merge[0].second) {
        src_level->AddSSTable(std::move(reloaded));   // 第一个是源文件
      } else {
        dst_level->AddSSTable(std::move(reloaded));
      }
    }
    std::cerr << "Compaction rolled back due to file deletion failure.\n";
    return;
  }

  // 删除成功，将新文件加入目标层
  auto new_sst = std::make_unique<SSTable<K, V>>(new_filename);
  dst_level->AddSSTable(std::move(new_sst));

  PersistManifest();    // 一次持久化

  std::cout << "[Compaction] Merged level " << level << " into level " << next
            << ", new file: " << new_filename << " (" << merged.size() << " entries)\n";
}

// 强制全量合并：将 L0 的所有文件依次合并到 L1，最终所有数据推至最低层
template<typename K, typename V>
void LeveledCompaction<K, V>::ForceCompaction() {
  while (!levels_[0]->GetFiles().empty()) {
    DoCompact(0, 0);
  }
}

// 打印各层状态信息（文件数、字节数/容量）
template<typename K, typename V>
void LeveledCompaction<K, V>::PrintStats() const {
  std::lock_guard<std::mutex> lock(mu_);
  for (int i = 0; i < max_level_; ++i) {
    std::cout << "Level " << i << ": " << levels_[i]->GetFiles().size() << " files, "
              << levels_[i]->TotalBytes() << " / " << level_sizes_[i] << " bytes\n";
  }
}

// 获取全局所有 SSTable 的迭代器（按版本从新到旧排序），用于全量扫描
template<typename K, typename V>
std::vector<std::unique_ptr<IteratorInterface<K, V>>> LeveledCompaction<K, V>::GetAllIterators() const {
  std::lock_guard<std::mutex> lock(mu_);
  // 收集所有 SSTable 及其版本号
  std::vector<std::pair<uint64_t, const SSTable<K, V>*>> sst_with_seq;
  for (int lvl = 0; lvl < max_level_; ++lvl) {
    for (const auto &sst : levels_[lvl]->GetFiles()) {
      // 从文件名提取版本号
      std::string fname = sst->Filename();
      size_t slash = fname.find_last_of('/');
      if (slash != std::string::npos)
        fname = fname.substr(slash + 1);
      size_t dot = fname.find('.');
      if (dot != std::string::npos)
        fname = fname.substr(0, dot);
      uint64_t seq = std::stoull(fname);
      sst_with_seq.emplace_back(seq, sst.get());
    }
  }
  // 按版本降序排序（新的在前）
  std::sort(sst_with_seq.begin(), sst_with_seq.end(),
            [](const auto &a, const auto &b) { return a.first > b.first; });

  // 创建包装迭代器列表
  std::vector<std::unique_ptr<IteratorInterface<K, V>>> iters;
  for (auto &p : sst_with_seq) {
    auto sst_iter = p.second->NewIterator();
    if (sst_iter.Valid()) {
      iters.push_back(std::make_unique<SSTableIteratorWrapper<K, V>>(std::move(sst_iter)));
    }
  }
  return iters;
}

/**
 * 从 SSTable 目录加载数据，优先从 Manifest 文件恢复层级结构；
 * 若 Manifest 不存在或损坏，则扫描所有 .sst 文件放入 L0，并强制执行一次全量合并。
 * 该函数应在系统启动时调用一次
 */
template<typename K, typename V>
void LeveledCompaction<K, V>::LoadFromDir() {
  std::lock_guard<std::mutex> lock(mu_);   // 保护 levels_ 等成员

  uint64_t last_seq;
  std::vector<std::vector<FileMeta<K>>> loaded_mates;

  // 尝试从 Manifest 恢复层级信息
  if (Manifest::Load(sst_dir_, loaded_mates, last_seq)) {

    // 清空所有现有层级数据，按 Manifest 内容重建
    for (int i = 0; i < max_level_; ++i) {
      levels_[i]->Clear();                       // 清空该层
      if (i < static_cast<int>(loaded_mates.size())) {
        // 将 Manifest 中记录的该层每个 SSTable 重新打开并加入
        for (const auto &meta : loaded_mates[i]) {
          std::string full_path = sst_dir_ + "/" + meta.filename;
          if (fs::exists(full_path)) {
            auto sst = std::make_unique<SSTable<K, V>>(full_path);
            levels_[i]->AddSSTable(std::move(sst));
          } else {
            std::cerr << "Warning: SSTable " << full_path << " missing\n";
          }
        }
      }
    }
    std::cout << "Loaded from Manifest, last sequence: " << last_seq << std::endl;

  } else {
    // Manifest 不可用，回退到扫描目录方式
    std::cerr << "No valid Manifest found, scanning directory...\n";

    // 先清空所有层
    for (auto &lvl : levels_) {
      lvl->Clear();
    }

    // 扫描目录下所有 .sst 文件，全部作为 L0 文件
    if (fs::exists(sst_dir_)) {
      for (const auto &entry : fs::directory_iterator(sst_dir_)) {
        if (entry.path().extension() != ".sst")
          continue;
        auto sst = std::make_unique<SSTable<K, V>>(entry.path().string());
        levels_[0]->AddSSTable(std::move(sst));   // 放入 L0
      }
    }

    // 强制执行全量合并，将 L0 数据逐渐推至合适层级
    ForceCompaction();

    // 合并完成后生成新的 Manifest 文件
    PersistManifest();
  }
}

// ---------------------- 实现后台合并循环 ----------------------

/**
 * 后台合并线程的主循环。
 * 等待合并信号（need_compact_）或停止信号（stop_），每次唤醒后检查各层容量，
 * 对超限的层级执行一次 Compaction，然后持久化 Manifest。
 * 执行完一次合并后，若可能还有未处理的超限层，会再次触发合并信号。
 */
template<typename K, typename V>
void LeveledCompaction<K, V>::BackgroundCompactLoop() {
  while (true) {
    std::unique_lock<std::mutex> lk(compact_mu_);
    // 等待条件：need_compact_ 为 true 或 stop_ 为 true
    cv_.wait(lk, [this] { return need_compact_ || stop_; });
    if (stop_)           // 收到停止信号，退出循环
      break;
    need_compact_ = false;   // 重置合并请求标志
    lk.unlock();             // 释放 compact_mu_，避免长时间持有

    bool did_work = false;
    {
      // 获取主锁 mu_，保护 levels_ 等数据结构
      std::lock_guard<std::mutex> main_lock(mu_);

      // 从 L0 到最高层遍历，寻找超限的层
      for (int level = 0; level < max_level_; ++level) {
        auto &lvl = levels_[level];
        if (lvl->ExceedsLimit()) {
          // L0 使用特殊策略选择文件，L1+ 直接选第一个文件（文件已有序）
          size_t file_index = (level == 0) ? PickFileToCompactFromL0() : 0;
          if (file_index < lvl->GetFiles().size()) {
            DoCompact(level, file_index);   // 执行合并
            PersistManifest();              // 合并后立即保存 Manifest
            did_work = true;
            break;  // 每次循环只做一个合并，释放锁后再检查后续需求
          }
        }
      }
    }

    // 如果本次执行了合并，可能还有其他层需要处理，再次触发合并信号
    if (did_work) {
      SignalCompact();
    }
  }
}

/**
 * 将当前所有层级的元数据（文件列表、键范围等）保存到 MANIFEST 文件。
 * 内部调用 Manifest::Save 进行原子持久化。
 * @return 成功返回 true，否则 false
 */
template<typename K, typename V>
bool LeveledCompaction<K, V>::PersistManifest() {
  // 收集每一层的 FileMeta 信息
  std::vector<std::vector<FileMeta<K>>> all_metas;
  all_metas.reserve(max_level_);
  for (int i = 0; i < max_level_; ++i) {
    std::vector<FileMeta<K>> metas;
    levels_[i]->GetAllFileMeta(metas);
    all_metas.push_back(std::move(metas));
  }
  // 委托 Manifest 工具类写入磁盘
  return Manifest::Save(sst_dir_, all_metas);
}

} // namespace leveled

#endif // CVSTORE_INC_LEVELED_COMPACTION_H_