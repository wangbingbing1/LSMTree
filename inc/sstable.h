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

namespace fs = std::filesystem;

// ======================== 二进制序列化工具 ========================

// 通用 POD 类型写入（如 int, uint64_t）
template<typename T>
typename std::enable_if<std::is_pod<T>::value>::type
WriteBinary(std::ostream &os, const T &value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

// 通用 POD 类型读取
template<typename T>
typename std::enable_if<std::is_pod<T>::value>::type
ReadBinary(std::istream &is, T &value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

// std::string 特化：先写 4 字节长度，再写字符数据
inline void WriteBinary(std::ostream &os, const std::string &str) {
  uint32_t len = static_cast<uint32_t>(str.size());
  WriteBinary(os, len);            // 写入长度
  os.write(str.data(), len);       // 写入内容
}

// std::string 特化：先读长度，再读指定字节
inline void ReadBinary(std::istream &is, std::string &str) {
  uint32_t len;
  ReadBinary(is, len);
  str.resize(len);
  is.read(&str[0], len);
}

// 写入 1 字节的墓碑标记（0=正常，1=墓碑）
inline void WriteFlags(std::ostream &os, bool is_tombstone) {
  uint8_t flags = is_tombstone ? 1 : 0;
  WriteBinary(os, flags);
}

// 读取 1 字节标记并解析为布尔值
inline bool ReadFlags(std::istream &is) {
  uint8_t flags;
  ReadBinary(is, flags);
  return (flags & 1) != 0;
}

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

    // 写入 Footer：索引区的起始偏移量（8字节）
    uint64_t index_offset = offset;
    WriteBinary(out, index_offset);

    out.close();
    return static_cast<bool>(out);
  }

  // 打开已有 SSTable 文件，加载索引到内存
  explicit SSTable(const std::string &filename) : filename_(filename), file_size_(0) {
    // 获取文件大小
    std::ifstream in(filename, std::ios::binary | std::ios::ate);
    if (in.is_open()) {
      file_size_ = in.tellg();
      in.close();
    }
    LoadIndex();   // 从文件加载索引区
  }

  // 禁止拷贝（文件资源独占），允许移动
  SSTable(const SSTable &) = delete;
  SSTable &operator=(const SSTable &) = delete;
  SSTable(SSTable &&) = default;
  SSTable &operator=(SSTable &&) = default;

  // 点查询：通过索引找到偏移，读取值（跳过墓碑）
  bool Get(const K &key, V &value) const {
    auto it = index_.find(key);
    if (it == index_.end()) return false;

    uint64_t offset = it->second;          // 数据区偏移

    std::ifstream in(filename_, std::ios::binary);
    if (!in.is_open()) return false;

    in.seekg(offset, std::ios::beg);
    bool tombstone = ReadFlags(in);        // 读取墓碑标记
    K dummy_key;
    ReadBinary(in, dummy_key);             // 跳过键

    if (tombstone) return false;           // 墓碑 → 不存在

    ReadBinary(in, value);                 // 读取值
    return true;
  }

  // 判断索引中是否包含某键（用于查询终止）
  bool HasKey(const K &key) const {
    return index_.find(key) != index_.end();
  }

  size_t FileSize() const { return file_size_; }
  const std::string &Filename() const { return filename_; }

 private:
  std::string filename_;                   // 文件路径
  size_t file_size_;                       // 文件大小（字节）
  std::map<K, uint64_t> index_;            // 内存索引：键 → 数据偏移量

  // 从文件的索引区加载所有 (key, offset) 到 index_
  void LoadIndex() {
    std::ifstream in(filename_, std::ios::binary);
    if (!in.is_open()) {
      std::cerr << "Failed to open SSTable for index loading: " << filename_ << std::endl;
      return;
    }

    // 1. 读取 Footer（最后8字节）
    in.seekg(-8, std::ios::end);
    uint64_t index_offset;
    ReadBinary(in, index_offset);

    // 2. 跳转到索引区
    in.seekg(index_offset, std::ios::beg);

    // 3. 读取条目数
    uint32_t index_size;
    ReadBinary(in, index_size);

    // 4. 逐条读入
    for (uint32_t i = 0; i < index_size; ++i) {
      K key;
      ReadBinary(in, key);
      uint64_t offset;
      ReadBinary(in, offset);
      index_[key] = offset;
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

  std::vector<std::unique_ptr<SSTable<K, V>>>& GetTables(){return tables_;}
  const std::vector<std::unique_ptr<SSTable<K, V>>>& GetTables()const{return tables_;}
 private:
  // 按加入顺序保存（旧→新），查询时逆序
  std::vector<std::unique_ptr<SSTable<K, V>>> tables_;
};

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