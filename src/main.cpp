//
// Created by wangbingbing on 2026/5/20.
//
// 主程序：键值存储引擎的命令行入口


#include <iostream>
#include <string>
#include <fstream>
#include <cerrno>
#include "skip_list.h"
#include "sstable.h"
#include "merging_iterator.h"
#include "crc32.h"
#include "leveled_compaction.h"

using KVStore = SkipList<std::string, std::string>;

// ---------------------------- 全局对象 ----------------------------

// 分层合并管理器（全局单例）
// 参数：SSTable 目录、最大层数（L0~L3）、各层容量上限（字节）
static leveled::LeveledCompaction<std::string, std::string> g_compactor(
    "./sstables",
    4,
    {
        8 * 1024 * 1024,      // L0:  8 MB
        32 * 1024 * 1024,     // L1: 32 MB
        256 * 1024 * 1024,    // L2: 256 MB
        1024 * 1024 * 1024    // L3: 1024 MB
    });

static std::string g_sst_dir = "./sstables";   // SSTable 文件存放目录

// ---------------------------- 辅助函数 ----------------------------

/**
 * 将内存表（MemTable）中的所有数据刷写到 SSTable 文件，并加入分层合并管理器。
 * 同时清空内存表和 WAL 文件。
 *
 * @param db       内存表（跳表）
 * @param wal      WAL 输出流（会被关闭并清空）
 * @param sst_dir  SSTable 目录
 * @param compactor 分层合并管理器
 * @return         成功返回 true，失败返回 false
 */
static bool FlushToCompactor(SkipList<std::string, std::string> &db,
                             std::ofstream &wal,
                             const std::string &sst_dir,
                             leveled::LeveledCompaction<std::string, std::string> &compactor) {
  // 收集内存表中所有数据（包括墓碑）
  std::vector<std::tuple<std::string, std::string, bool>> data;
  db.ForEachWithTombstone([&data](const std::string &k, const std::string &v, bool tombstone) {
    data.emplace_back(k, v, tombstone);
  });
  if (data.empty()) return true;   // 无数据无需刷写

  // 生成新的 SSTable 文件名（编号递增）
  int file_num = GetNextSSTableNumber(sst_dir);
  std::string filename = sst_dir + "/" + std::to_string(file_num) + ".sst";
  if (!SSTable<std::string, std::string>::Build(filename, data)) {
    std::cerr << "Failed to build SSTable: " << filename << "\n";
    return false;
  }

  // 将新 SSTable 加入分层管理器（放入 L0 层）
  auto sst = std::make_unique<SSTable<std::string, std::string>>(filename);
  compactor.AddSSTable(std::move(sst));

  // 清空内存表
  db.Clear();

  // 清空 WAL 文件（关闭 → 截断 → 重新以追加模式打开）
  wal.close();
  std::ofstream clear_wal("wal.log", std::ios::trunc);
  clear_wal.close();
  wal.open("wal.log", std::ios::app);

  std::cout << "[Flush] Created SSTable: " << filename
            << " with " << data.size() << " entries.\n";
  return true;
}

/**
 * 自动检查是否需要触发 Flush（基于内存表大小或 WAL 文件大小）。
 * 该函数会在每次写操作（put/del/commit）后被调用。
 *
 * @param db            内存表
 * @param wal           WAL 输出流
 * @param max_mem_size  内存表记录数阈值（默认 10 万条）
 * @param max_wal_size   WAL 文件大小阈值（默认 1MB）
 */
void AutoCheckpoint(SkipList<std::string, std::string> &db,
                    std::ofstream &wal,
                    size_t max_mem_size = 100000,
                    size_t max_wal_size = 1024 * 1024) {
  static size_t last_checkpoint_size = 0;   // 上次 Flush 时内存表的大小

  // 条件1：自上次 Flush 以来新增数据量超过阈值
  if (db.Size() - last_checkpoint_size > max_mem_size) {
    if (FlushToCompactor(db, wal, g_sst_dir, g_compactor)) {
      last_checkpoint_size = db.Size();   // 重置基准（Flush 后 db.Size() 通常为 0）
    }
    return;
  }

  // 条件2：WAL 日志文件物理大小超过阈值
  std::ifstream wal_file("wal.log", std::ios::ate | std::ios::binary);
  if (wal_file.is_open()) {
    size_t size = wal_file.tellg();
    if (size > max_wal_size) {
      if (FlushToCompactor(db, wal, g_sst_dir, g_compactor)) {
        last_checkpoint_size = db.Size();
      }
    }
  }
}

// ---------------------------- 主函数 ----------------------------

int main() {
  // ========== 1. 启动阶段：恢复持久化数据 ==========

  // 1.1 加载已有的 SSTable 文件（从磁盘恢复到分层管理器中）
  g_compactor.LoadFromDir();

  // 1.2 创建内存表（跳表）
  KVStore db;

  // 1.3 从 WAL 日志中恢复未刷盘的数据
  std::ifstream recover_file("wal.log", std::ios::binary);
  if (recover_file.is_open()) {
    bool in_batch = false;
    WriteBatch<std::string, std::string> recovery_batch;
    bool error_occurred = false;
    std::streampos last_good_pos = 0;               // 最后一个完整记录的结束位置
    const size_t MAX_KEY_LEN = 1024 * 1024;          // 最大键长 1MB
    const size_t MAX_VALUE_LEN = 1024 * 1024;        // 最大值长 1MB

    while (recover_file.peek() != EOF) {
      uint8_t type;
      recover_file.read(reinterpret_cast<char*>(&type), 1);
      if (!recover_file) break;

      // 批量操作边界标记
      if (type == 2) {          // BEGIN_BATCH
        in_batch = true;
        recovery_batch.Clear();
        last_good_pos = recover_file.tellg();
        continue;
      } else if (type == 3) {   // COMMIT_BATCH
        if (in_batch) {
          db.ApplyBatchWithoutWAL(recovery_batch);   // 将批次应用到内存表
          in_batch = false;
        }
        last_good_pos = recover_file.tellg();
        continue;
      }

      // 读取 key 长度
      uint32_t key_len;
      recover_file.read(reinterpret_cast<char*>(&key_len), 4);
      if (!recover_file) {
        error_occurred = true;
        break;
      }
      if (key_len == 0 || key_len > MAX_KEY_LEN) {
        std::cerr << "WAL: invalid key_len=" << key_len << ", truncating at offset "
                  << last_good_pos << std::endl;
        error_occurred = true;
        break;
      }

      // 读取 key
      std::string key(key_len, '\0');
      recover_file.read(&key[0], key_len);
      if (!recover_file) {
        error_occurred = true;
        break;
      }

      // 读取 value 长度
      uint32_t value_len;
      recover_file.read(reinterpret_cast<char*>(&value_len), 4);
      if (!recover_file) {
        error_occurred = true;
        break;
      }
      if (value_len > MAX_VALUE_LEN) {
        std::cerr << "WAL: invalid value_len=" << value_len << ", truncating.\n";
        error_occurred = true;
        break;
      }

      // 读取 value（如果存在）
      std::string value(value_len, '\0');
      if (value_len > 0) {
        recover_file.read(&value[0], value_len);
        if (!recover_file) {
          error_occurred = true;
          break;
        }
      }

      // 读取存储的 CRC32 校验值
      uint32_t stored_crc;
      recover_file.read(reinterpret_cast<char*>(&stored_crc), 4);
      if (!recover_file) {
        error_occurred = true;
        break;
      }

      // 校验 CRC32
      std::string buffer;
      buffer.append(reinterpret_cast<const char*>(&type), 1);
      buffer.append(reinterpret_cast<const char*>(&key_len), 4);
      buffer.append(key.data(), key_len);
      buffer.append(reinterpret_cast<const char*>(&value_len), 4);
      if (value_len > 0) buffer.append(value.data(), value_len);
      uint32_t calc_crc = CRC32(buffer.data(), buffer.size());
      if (calc_crc != stored_crc) {
        std::cerr << "WAL checksum mismatch at key=" << key << ", truncating.\n";
        error_occurred = true;
        break;
      }

      // 记录当前记录的结束位置（成功通过校验）
      last_good_pos = recover_file.tellg();

      // 将记录应用到内存表
      if (type == 0) {   // PUT
        if (in_batch) recovery_batch.Put(key, value);
        else db.Insert(key, value);
      } else if (type == 1) {   // DELETE
        if (in_batch) recovery_batch.Delete(key);
        else db.Remove(key);
      }
    }

    recover_file.close();

    // 如果发生错误，截断 WAL 到最后有效位置，丢弃损坏部分
    if (error_occurred && last_good_pos > 0) {
      std::ofstream truncate_wal("wal.log", std::ios::binary | std::ios::in | std::ios::out);
      if (truncate_wal.is_open()) {
        truncate_wal.seekp(last_good_pos);
        truncate_wal.close();
      }
      std::cerr << "WAL truncated to last valid record. Some recent writes lost.\n";
    } else if (error_occurred) {
      // 没有任何有效记录，清空 WAL
      std::ofstream clear_wal("wal.log", std::ios::trunc);
      clear_wal.close();
      std::cerr << "WAL cleared due to corruption from start.\n";
    }

    std::cout << "Recovery finished, memtable size: " << db.Size() << std::endl;
  }

  // 1.4 以追加模式打开 WAL，用于记录本次运行的所有写操作
  std::ofstream wal("wal.log", std::ios::app);

  // 批处理相关变量
  WriteBatch<std::string, std::string> batch;
  bool in_batch = false;
  std::string cmd, key, value;

  // ========== 2. 交互式命令行循环 ==========
  std::cout << "KVStore ready. Type 'help' for commands.\n";
  while (true) {
    std::cout << (in_batch ? "[batch]" : ">");
    std::cin >> cmd;

    // ---------- 事务控制 ----------
    if (cmd == "begin") {
      if (in_batch) {
        std::cout << "Already in a batch, commit or rollback first.\n";
      } else {
        batch.Clear();
        in_batch = true;
        std::cout << "Batch started.\n";
      }
    } else if (cmd == "commit") {
      if (!in_batch) {
        std::cout << "No active batch.\n";
      } else {
        // 将批次中的所有操作原子写入 WAL 并应用到内存表
        db.ApplyBatch(batch, wal);
        AutoCheckpoint(db, wal);   // 检查是否需要 Flush
        in_batch = false;
        std::cout << "Batch committed.\n";
      }
    } else if (cmd == "rollback") {
      if (!in_batch) {
        std::cout << "No active batch.\n";
      } else {
        batch.Clear();
        in_batch = false;
        std::cout << "Batch rolled back.\n";
      }
    }

      // ---------- 写入操作 ----------
    else if (cmd == "put") {
      std::cin >> key >> value;
      if (in_batch) {
        batch.Put(key, value);
        std::cout << "Added to batch.\n";
      } else {
        // 先写 WAL 保证持久性
        WritePutToWAL(wal, key, value);
        wal.flush();
        // 再写入内存表
        db.Insert(key, value);
        AutoCheckpoint(db, wal);
        std::cout << "OK!\n";
      }
    }

      // ---------- 删除操作 ----------
    else if (cmd == "del") {
      std::cin >> key;
      if (in_batch) {
        batch.Delete(key);
        std::cout << "Added to batch.\n";
      } else {
        WriteDeleteToWAL(wal, key);
        wal.flush();
        db.Remove(key);      // 内存表中插入墓碑
        AutoCheckpoint(db, wal);
        std::cout << "Deleted.\n";
      }
    }

      // ---------- 点查询 ----------
    else if (cmd == "get") {
      std::cin >> key;
      std::string val;
      // 查询顺序：MemTable → SSTable（分层管理器）
      if (db.Contains(key)) {
        if (db.Get(key, val)) {
          std::cout << val << "\n";
        } else {
          std::cout << "NOT FOUND!\n";
        }
      } else if (g_compactor.Get(key, val)) {
        std::cout << val << " (from SSTable)\n";
      } else {
        std::cout << "NOT FOUND!\n";
      }
    }

      // ---------- 范围扫描 ----------
    else if (cmd == "scan") {
      std::string start, end;
      std::cin >> start >> end;

      // 收集所有数据源迭代器
      std::vector<std::unique_ptr<IteratorInterface<std::string, std::string>>> iters;

      // 1. MemTable 迭代器（最新数据）
      auto mem_iter = db.GetIterator();
      iters.push_back(std::make_unique<SkipListIteratorWrapper<std::string, std::string>>(std::move(mem_iter)));

      // 2. 分层管理器中所有 SSTable 迭代器（按文件生成时间倒序）
      auto sst_iters = g_compactor.GetAllIterators();
      for (auto &it : sst_iters) {
        iters.push_back(std::move(it));
      }

      // 创建归并迭代器（自动去重、跳过墓碑）
      MergingIterator<std::string, std::string> merge_iter(std::move(iters));

      // 定位到范围起始键
      while (merge_iter.Valid() && merge_iter.Key() < start) {
        merge_iter.Next();
      }

      // 输出范围内的所有键值对
      bool has_output = false;
      while (merge_iter.Valid() && merge_iter.Key() <= end) {
        std::cout << merge_iter.Key() << ":" << merge_iter.Value() << std::endl;
        has_output = true;
        merge_iter.Next();
      }
      if (!has_output) {
        std::cout << "No keys in range [" << start << ", " << end << "]\n";
      }
    }

      // ---------- 手动刷写（Flush）----------
    else if (cmd == "save" || cmd == "flush") {
      if (in_batch) {
        std::cout << "Cannot save while in batch.\n";
        continue;
      }
      if (FlushToCompactor(db, wal, g_sst_dir, g_compactor)) {
        std::cout << "Saved (flushed) and WAL cleared.\n";
      } else {
        std::cerr << "Save failed.\n";
      }
    }

      // ---------- 查看统计信息 ----------
    else if (cmd == "stats") {
      g_compactor.PrintStats();
    }

      // ---------- 手动触发合并 ----------
    else if (cmd == "compact") {
      if (in_batch) {
        std::cout << "Cannot compact while in batch.\n";
      } else {
        std::cout << "Starting manual compaction...\n";
        g_compactor.ForceCompaction();
        std::cout << "Manual compaction completed.\n";
      }
    }

      // ---------- 退出系统 ----------
    else if (cmd == "quit") {
      if (!in_batch) {
        // 退出前执行一次 Flush，确保所有数据持久化到 SSTable
        FlushToCompactor(db, wal, g_sst_dir, g_compactor);
      } else {
        std::cout << "Batch is active, rollback or commit before quit.\n";
        continue;
      }
      wal.close();
      std::cout << "Quit, data flushed to SSTables.\n";
      break;
    }

      // ---------- 帮助信息 ----------
    else {
      std::cout << "Commands:\n"
                << "  put key value         - insert/update key with value\n"
                << "  del key               - delete key (tombstone)\n"
                << "  get key               - retrieve value for key\n"
                << "  scan start end        - list all keys in [start, end]\n"
                << "  begin                 - start a batch\n"
                << "  commit                - commit the batch\n"
                << "  rollback              - abort the batch\n"
                << "  flush / save          - force flush memtable to SSTable\n"
                << "  stats                 - show LSM tree layer statistics\n"
                << "  compact               - force full compaction (merge all L0 files)\n"
                << "  quit                  - exit the program\n";
    }
  }

  return 0;
}