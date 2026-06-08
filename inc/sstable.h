//
// Created by wangbingbing on 2026/5/23.
//
// SSTable 实现：不可变持久化键值存储文件，支持索引快速定位和墓碑记录。

#ifndef TEST3_INC_SSTABLE_H_
#define TEST3_INC_SSTABLE_H_

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <fstream>
#include <iostream>
#include <cstring>
#include <type_traits>
#include <filesystem>

#include "skip_list.h"
#include "bloom_filter.h"
#include "block_cache.h"
#include "binary_io.h"

namespace fs = std::filesystem;

// ======================== SSTable ========================
template<typename K, typename V>
class SSTable {
 public:
  // 构建一个 SSTable 文件
  // data 是 (key, value, is_tombstone) 的有序集合 按键升序
  static bool Build(const std::string &filename,
                    const std::vector<std::tuple<K, V, bool>> &data) {
    std::ofstream out(filename, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      std::cerr << "Failed to create SSTable file: " << filename << std::endl;
      return false;
    }

    std::map<K, uint64_t> index;   // 键 → 数据区偏移
    uint64_t offset = 0;

    // 写入数据区
    for (const auto &entry : data) {
      const K &key = std::get<0>(entry);
      const V &value = std::get<1>(entry);
      bool tombstone = std::get<2>(entry);

      index[key] = offset;                 // 记录该键的记录起始偏移

      WriteFlags(out, tombstone);          // 1字节墓碑标记
      WriteBinary(out, key);               // 键（变长序列化）

      if (!tombstone) {
        WriteBinary(out, value);           // 值（仅非墓碑时写入）
      }

      offset = out.tellp();                // 更新下一次记录的偏移
    }

    // 写入索引区：条目数 + 每条 (key, offset)
    uint32_t index_size = static_cast<uint32_t>(index.size());
    WriteBinary(out, index_size);
    for (const auto &entry : index) {
      WriteBinary(out, entry.first);       // 键
      WriteBinary(out, entry.second);      // 偏移量（8字节）
    }

    // --- 布隆过滤器（基于所有有效键）---
    BloomFilter<K> filter(10, index.size()); // bits_per_key = 10
    for (const auto &entry : data) {
      if (!std::get<2>(entry)) {             // 非墓碑键
        filter.Add(std::get<0>(entry));
      }
    }
    std::string filter_data = filter.Serialize();
    uint64_t bloom_offset = out.tellp();
    uint32_t filter_size = static_cast<uint32_t>(filter_data.size());
    WriteBinary(out, filter_size);
    out.write(filter_data.data(), filter_size);

    // 写入 Footer：索引区的起始偏移量（8字节）布隆过滤器偏移 (8字节)
    uint64_t index_offset = offset;
    WriteBinary(out, index_offset);
    WriteBinary(out, bloom_offset);

    out.close();
    return static_cast<bool>(out);
  }

  // 打开已有 SSTable 文件，加载索引到内存
  explicit SSTable(const std::string &filename)
      : filename_(filename), file_size_(0), bloom_filter_(10, 1000), index_offset_(0) {
    // 获取文件大小
    std::ifstream in(filename, std::ios::binary | std::ios::ate);
    if (in.is_open()) {
      file_size_ = in.tellg();
      in.close();
    }
    LoadIndexAndFilter();   // 从文件加载索引区
    file_stream_.open(filename_, std::ios::binary);
    if (!file_stream_.is_open()) {
      std::cerr << "Warning: failed to open SSTable file for caching: " << filename_ << std::endl;
    }

    if (!index_.empty()) {
      first_key_ = index_.begin()->first;
      last_key_ = index_.rbegin()->first;
      has_keys_ = true;
    }
  }

  // 禁止拷贝（文件资源独占），允许移动
  SSTable(const SSTable &) = delete;
  SSTable &operator=(const SSTable &) = delete;
  SSTable(SSTable &&other) noexcept: filename_(other.filename_),
                                     file_size_(other.file_size_),
                                     index_(std::move(other.index_)),
                                     bloom_filter_(std::move(other.bloom_filter_)),
                                     index_offset_(other.index_offset_),
                                     first_key_(std::move(other.first_key_)),
                                     last_key_(std::move(other.last_key_)),
                                     has_keys_(other.has_keys_),
                                     file_stream_(std::move(other.file_stream_)) {
    other.file_size_ = 0;
    other.index_offset_ = 0;
    other.has_keys_ = false;
  }
  SSTable &operator=(SSTable &&) = default;

  // 点查询：先用布隆过滤器快速判断，再查找索引并读取值
  bool Get(const K &key, V &value) const {

    // 所有SSTable共享cache 容量为64MB
    static BlockCache<V> cache(64 * 1024 * 1024);

    if (!bloom_filter_.MayContain(key))
      return false;

    auto it = index_.find(key);
    if (it == index_.end())
      return false;

    uint64_t offset = it->second;          // 数据区偏移

    std::string cache_key = filename_ + ":" + std::to_string(offset);

    if (cache.Lookup(cache_key, value)) {
      return true; // 命中
    }

    // 使用缓存的文件流
    if (!file_stream_.is_open()) {
      // 若流已关闭 重新打开
      file_stream_.open(filename_, std::ios::binary);
      if (!file_stream_.is_open())
        return false;
    }
    // 清除错误状态并定位到偏移
    file_stream_.clear();
    file_stream_.seekg(offset, std::ios::beg);
    if (file_stream_.fail())
      return false;

    bool tombstone = ReadFlags(file_stream_);        // 读取墓碑标记
    K dummy_key;
    ReadBinary(file_stream_, dummy_key);             // 跳过键

    if (tombstone)
      return false;           // 墓碑 → 不存在

    ReadBinary(file_stream_, value);                 // 读取值
    cache.Insert(cache_key, value);  // 写入缓存
    return true;
  }

  // 判断索引中是否包含某键（用于查询终止）
  bool HasKey(const K &key) const {
    return index_.find(key) != index_.end();
  }

  size_t FileSize() const { return file_size_; }
  const std::string &Filename() const { return filename_; }

  class Iterator {
   public:
    // 构造函数：接受文件流右值和数据区结束偏移
    Iterator(std::ifstream &&stream, uint64_t data_end_offset);
    ~Iterator();

    Iterator(Iterator &&) = default;
    Iterator &operator=(Iterator &&) = default;

    Iterator(const Iterator &) = delete;
    Iterator &operator=(const Iterator &) = delete;

    bool Valid() const { return valid_; }
    void Next();
    const K &Key() const { return key_; }
    const V &Value() const { return value_; }
    bool IsTombstone() const { return tombstone_; }

   private:
    std::ifstream file_;
    uint64_t data_end_offset_;
    uint64_t current_offset_;
    bool valid_;
    K key_;
    V value_;
    bool tombstone_;

    bool ReadNext();// 读取下一条记录，返回是否成功
  };

  bool GetFirstKey(K &key) const {
    if (!has_keys_)return false;
    key = first_key_;
    return true;
  }
  bool GetLastKey(K &key) const {
    if (!has_keys_)return false;
    key = last_key_;
    return true;
  }
  Iterator NewIterator() const {
    std::ifstream in(filename_, std::ios::binary);
    if (!in.is_open())
      return Iterator(std::move(in), 0);
    return Iterator(std::move(in), index_offset_);
  }

  void CloseFile(){
    if(file_stream_.is_open()){
      file_stream_.close();
    }
  }

 private:
  std::string filename_;                   // 文件路径
  size_t file_size_;                       // 文件大小（字节）
  std::map<K, uint64_t> index_;            // 内存索引：键 → 数据偏移量
  BloomFilter<K> bloom_filter_;
  uint64_t index_offset_;                   //  数据区偏移索引
  K first_key_;
  K last_key_;
  bool has_keys_ = false;
  mutable std::ifstream file_stream_;       // 可变的文件流，用于 Get 操作

  // 从文件的索引区加载所有 (key, offset) 到 index_
  void LoadIndexAndFilter() {
    std::ifstream in(filename_, std::ios::binary);
    if (!in.is_open()) {
      std::cerr << "Failed to open SSTable for index loading: " << filename_ << std::endl;
      return;
    }

    // 获取文件大小，确保至少 16 字节 Footer
    in.seekg(0, std::ios::end);
    size_t file_size = in.tellg();
    if (file_size < 16) {
      std::cerr << "SSTable file too small: " << filename_ << std::endl;
      return;
    }

    // 1. 读取 Footer（最后 16 字节）
    in.seekg(-16, std::ios::end);
    uint64_t index_offset, bloom_offset;
    ReadBinary(in, index_offset);
    ReadBinary(in, bloom_offset);

    index_offset_ = index_offset;

    // 2. 加载索引区
    if (index_offset >= file_size) {
      std::cerr << "Invalid index offset in " << filename_ << std::endl;
      return;
    }
    in.seekg(index_offset, std::ios::beg);
    uint32_t index_size;
    ReadBinary(in, index_size);
    for (uint32_t i = 0; i < index_size; ++i) {
      K key;
      ReadBinary(in, key);
      uint64_t offset;
      ReadBinary(in, offset);
      index_[key] = offset;
    }

    // 3. 加载布隆过滤器（如果存在）
    if (bloom_offset != 0 && bloom_offset < file_size) {
      in.seekg(bloom_offset, std::ios::beg);
      uint32_t filter_size;
      ReadBinary(in, filter_size);
      std::string filter_data(filter_size, '\0');
      if (!in.read(&filter_data[0], filter_size)) {
        std::cerr << "Failed to read bloom filter data from " << filename_ << std::endl;
        bloom_filter_ = BloomFilter<K>(10, 1000);  // fallback
      } else {
        bloom_filter_ = BloomFilter<K>::Deserialize(filter_data);
      }
    } else {
      // 文件中没有布隆过滤器，构造一个默认的
      bloom_filter_ = BloomFilter<K>(10, 1000);
    }

    if (!in) {
      std::cerr << "Error reading index from SSTable: " << filename_ << std::endl;
    }
  }
};

// ======================== SSTableManager ========================
template<typename K, typename V>
class SSTableManager {
 public:
  // 添加一个 SSTable（移动所有权）
  void AddSSTable(std::unique_ptr<SSTable<K, V>> sst) {
    tables_.push_back(std::move(sst));
  }

  // 多级查询：从最新到最旧遍历，遇到第一个包含键的 SSTable 立即返回结果（墓碑则返回 false）
  bool Get(const K &key, V &value) const {
    for (auto it = tables_.rbegin(); it != tables_.rend(); ++it) {
      if ((*it)->HasKey(key)) {
        return (*it)->Get(key, value);
      }
    }
    return false;
  }

  size_t Size() const { return tables_.size(); }
  void Clear() { tables_.clear(); }

  // 返回所有 SSTable 的只读列表
  const std::vector<std::unique_ptr<SSTable<K, V>>> &GetALL() const {
    return tables_;
  }

  std::vector<std::unique_ptr<SSTable<K, V>>> &GetTables() { return tables_; }
  const std::vector<std::unique_ptr<SSTable<K, V>>> &GetTables() const { return tables_; }
 private:
  // 按加入顺序保存（旧→新），查询时逆序
  std::vector<std::unique_ptr<SSTable<K, V>>> tables_;
};

// ======================== SSTable::Iterator ========================
template<typename K, typename V>
SSTable<K, V>::Iterator::Iterator(std::ifstream &&stream, uint64_t data_end_offset):
    file_(std::move(stream)),
    data_end_offset_(data_end_offset),
    current_offset_(0),
    valid_(false) {
  if (file_.is_open()) {
    file_.seekg(0, std::ios::beg);
    valid_ = ReadNext();
  }
}

template<typename K, typename V>
SSTable<K, V>::Iterator::~Iterator() = default;

/**
 * 从 SSTable 数据区的当前位置读取下一条记录，并更新迭代器的状态。
 *
 * 一条记录在数据区的格式：
 *   [1 字节 flags] [key（变长序列化）] [value（仅当非墓碑时存在，变长序列化）]
 *
 * 该函数会读取并解析这些字段，然后更新 key_、value_、tombstone_，
 * 同时将 current_offset_ 移动到下一条记录的起始位置。
 *
 * @return true 如果成功读取一条记录，false 表示已到数据区末尾或发生读取错误。
 */
template<typename K, typename V>
bool SSTable<K, V>::Iterator::ReadNext() {
  // 文件未打开，无法读取
  if (!file_.is_open())
    return false;

  // 已经到达或超过数据区结束偏移，无更多记录
  if (current_offset_ >= data_end_offset_)
    return false;

  // 定位到当前记录的起始位置
  file_.seekg(current_offset_, std::ios::beg);

  // seekg 后流可能处于异常状态（如 EOF 或 fail）
  if (file_.eof() || file_.fail())
    return false;

  // 1. 读取 1 字节的墓碑标记
  uint8_t flags;
  ReadBinary(file_, flags);          // ReadBinary 读取单个 uint8_t
  bool tomb = (flags & 1) != 0;      // 位 0 为 1 表示墓碑

  // 2. 读取键（变长序列化，内部有长度前缀）
  K k;
  ReadBinary(file_, k);

  // 3. 读取值（仅当非墓碑时才存在）
  V v{};
  if (!tomb) {
    ReadBinary(file_, v);
  }

  // 4. 更新当前偏移量到这条记录之后（即下一条记录的开始位置）
  current_offset_ = file_.tellg();
  if (file_.fail())
    return false;                  // 获取偏移失败，返回 false

  // 5. 将读到的数据保存到迭代器成员中（通过移动语义，避免拷贝）
  key_ = std::move(k);
  value_ = std::move(v);
  tombstone_ = tomb;

  return true;
}

template<typename K, typename V>
void SSTable<K, V>::Iterator::Next() {
  if (!valid_)
    return;
  valid_ = ReadNext();
}

// ======================== 辅助函数 ========================

// 获取下一个 SSTable 文件编号（扫描目录，最大数字 + 1）
inline int GetNextSSTableNumber(const std::string &sst_dir) {
  int max_num = -1;

  // 目录不存在则创建
  if (!fs::exists(sst_dir)) {
    fs::create_directories(sst_dir);
    return 0;
  }

  // 遍历所有 .sst 文件，提取数字部分
  for (const auto &entry : fs::directory_iterator(sst_dir)) {
    if (entry.path().extension() == ".sst") {
      std::string stem = entry.path().stem().string();
      try {
        int num = std::stoi(stem);
        if (num > max_num) max_num = num;
      } catch (...) {
        // 忽略非数字文件名
      }
    }
  }
  return max_num + 1;
}

// 将内存跳表刷写到 SSTable，并清空 MemTable 和 WAL
template<typename K, typename V>
bool FlushMemTableToSSTable(SkipList<K, V> &db,
                            std::ofstream &wal,
                            const std::string &sst_dir,
                            SSTableManager<K, V> &sst_manager) {
  // 收集 MemTable 中所有数据（含墓碑）
  std::vector<std::tuple<K, V, bool>> data;
  db.ForEachWithTombstone([&data](const K &k, const V &v, bool tombstone) {
    data.emplace_back(k, v, tombstone);
  });
  if (data.empty()) return true;

  // 生成新文件名
  int file_num = GetNextSSTableNumber(sst_dir);
  std::string filename = sst_dir + "/" + std::to_string(file_num) + ".sst";

  // 构建 SSTable 文件
  if (!SSTable<K, V>::Build(filename, data)) {
    std::cerr << "Failed to build SSTable:" << filename << "\n";
    return false;
  }

  // 打开新 SSTable 并加入管理器
  auto sst = std::make_unique<SSTable<K, V>>(filename);
  sst_manager.AddSSTable(std::move(sst));

  // 清空 MemTable
  db.Clear();

  // 清空 WAL：关闭流 → 截断文件 → 重新以追加模式打开
  wal.close();
  std::ofstream clear_wal("wal.log", std::ios::trunc);
  clear_wal.close();
  wal.open("wal.log", std::ios::app);

  std::cout << "[Flush] Created SSTable: " << filename
            << " with " << data.size() << " entries.\n";
  return true;
}

// 启动时加载目录中所有 SSTable，按编号数字升序加入管理器（旧→新）
template<typename K, typename V>
void LoadSSTableFromDir(const std::string &sst_dir, SSTableManager<K, V> &sst_manager) {
  if (!fs::exists(sst_dir)) {
    fs::create_directories(sst_dir);
    return;
  }

  std::vector<std::string> sst_files;
  for (const auto &entry : fs::directory_iterator(sst_dir)) {
    if (entry.path().extension() == ".sst") {
      sst_files.push_back(entry.path().string());
    }
  }

  // 按文件编号数值升序排序
  std::sort(sst_files.begin(), sst_files.end(),
            [](const std::string &a, const std::string &b) {
              auto extract_num = [](const std::string &s) -> int {
                return std::stoi(fs::path(s).stem().string());
              };
              return extract_num(a) < extract_num(b);
            });

  // 依次加载
  for (const auto &file : sst_files) {
    auto sst = std::make_unique<SSTable<K, V>>(file);
    sst_manager.AddSSTable(std::move(sst));
    std::cout << "Loaded SSTable:" << file << std::endl;
  }
}

#endif //TEST3_INC_SSTABLE_H_