//
// Created by wangbingbing on 2026/5/20.
//

#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <cerrno>

#include "skip_list.h"
#include "sstable.h"
#include "compaction.h"

using KVStore = SkipList<std::string, std::string>;
using SSTableManagerType = SSTableManager<std::string, std::string>;

// 全局 SSTable 管理器和存储目录
static SSTableManagerType g_sst_manager;
static std::string g_sst_dir = "./sstables";

/**
 * 根据内存表大小或 WAL 文件大小自动触发 Flush
 * @param db            内存表（跳表）
 * @param wal           WAL 输出流
 * @param max_mem_size  触发 Flush 的内存增量阈值（默认 10 万条）
 * @param max_wal_size  触发 Flush 的 WAL 文件大小阈值（默认 1MB）
 */

void MaybeCompact();

void AutoCheckpoint(SkipList<std::string, std::string> &db,
                    std::ofstream &wal,
                    size_t max_mem_size = 100000,
                    size_t max_wal_size = 1024 * 1024) {
  static size_t last_checkpoint_size = 0;   // 上次 Flush 后 MemTable 的大小

  // 条件1：自上次 Flush 以来新增数据量超过阈值
  if (db.Size() - last_checkpoint_size > max_mem_size) {
    if (FlushMemTableToSSTable(db, wal, g_sst_dir, g_sst_manager)) {
      last_checkpoint_size = db.Size();     // 重置基准为当前大小（Flush 后通常为 0）
      MaybeCompact();
    }
    return;
  }

  // 条件2：WAL 日志文件物理大小超过阈值
  std::ifstream wal_file("wal.log", std::ios::ate | std::ios::binary);
  if (wal_file.is_open()) {
    size_t size = wal_file.tellg();
    if (size > max_wal_size) {
      if (FlushMemTableToSSTable(db, wal, g_sst_dir, g_sst_manager)) {
        last_checkpoint_size = db.Size();
      }
    }
  }
}

void MaybeCompact(){
  if (g_sst_manager.GetTables().size() >= 4) {
    CompactSSTable(g_sst_manager, g_sst_dir);
  }
}

int main() {
  // ---------- 启动阶段 ----------

  // 1. 加载已持久化的 SSTable 文件（按编号升序，旧→新）
  LoadSSTableFromDir(g_sst_dir, g_sst_manager);

  // 2. 创建内存表（MemTable）
  KVStore db;

  // 3. 从 WAL 恢复尚未持久化的操作
  std::ifstream recover_file("wal.log");
  if (recover_file.is_open()) {
    std::string line;
    while (std::getline(recover_file, line)) {
      if (line.empty()) continue;
      std::stringstream ss(line);
      std::string op, key, value;
      ss >> op;
      if (op == "PUT") {
        ss >> key >> value;
        db.Insert(key, value);         // 恢复普通写入
      } else if (op == "DEL") {
        ss >> key;
        db.Remove(key);               // 恢复删除（插入墓碑）
      } else {
        std::cerr << "Unknown WAL operation: " << op << std::endl;
      }
    }
    recover_file.close();

    // 恢复后立即清空 WAL，防止下次启动重复恢复
    std::ofstream clear_wal("wal.log", std::ios::trunc);
    clear_wal.close();

    std::cout << "Recovered from WAL, memtable size: " << db.Size() << std::endl;
  }

  // 4. 以追加模式打开 WAL，记录本次运行的所有写操作
  std::ofstream wal("wal.log", std::ios::app);

  // 批处理相关变量
  WriteBatch<std::string, std::string> batch;
  bool in_batch = false;
  std::string cmd, key, value;

  // ---------- 交互式命令行循环 ----------
  while (true) {
    std::cout << (in_batch ? "[batch]" : ">");
    std::cin >> cmd;

    // --- 事务控制 ---
    if (cmd == "begin") {
      if (in_batch) {
        std::cout << "Already in a batch, commit or rollback first.\n";
      } else {
        batch.Clear();
        in_batch = true;
        std::cout << "Batch started.\n";
      }
    }
    else if (cmd == "commit") {
      if (!in_batch) {
        std::cout << "No active batch.\n";
      } else {
        // 将批次应用到 MemTable 并先写 WAL
        db.ApplyBatch(batch, wal);
        AutoCheckpoint(db, wal);
        in_batch = false;
        std::cout << "Batch committed.\n";
      }
    }
    else if (cmd == "rollback") {
      if (!in_batch) {
        std::cout << "No active batch.\n";
      } else {
        batch.Clear();
        in_batch = false;
        std::cout << "Batch rolled back\n";
      }
    }

      // --- 写入操作 ---
    else if (cmd == "put") {
      std::cin >> key >> value;
      if (in_batch) {
        batch.Put(key, value);
        std::cout << "Added to batch.\n";
      } else {
        // 先写 WAL 保证持久性
        wal << "PUT " << key << " " << value << "\n";
        wal.flush();
        // 再写入内存表
        db.Insert(key, value);
        AutoCheckpoint(db, wal);
        std::cout << "OK!\n";
      }
    }

      // --- 删除操作 ---
    else if (cmd == "del") {
      std::cin >> key;
      if (in_batch) {
        batch.Delete(key);
        std::cout << "Added to batch.\n";
      } else {
        // WAL 记录删除
        wal << "DEL " << key << "\n";
        wal.flush();
        // db.Remove 会插入墓碑（is_tombstone=true），不物理删除节点
        db.Remove(key);
        AutoCheckpoint(db, wal);
        std::cout << "Deleted.\n";
      }
    }

      // --- 查询操作（点查）---
    else if (cmd == "get") {
      std::cin >> key;
      std::string val;
      // 查询顺序：MemTable → SSTable（从新到旧）
      if (db.Contains(key)) {                     // 先判断 MemTable 中是否有该键（包括墓碑）
        if (db.Get(key, val)) {                   // 存在且不是墓碑
          std::cout << val << "\n";
        } else {                                  // 存在但为墓碑
          std::cout << "NOT FOUND!\n";
        }
      } else if (g_sst_manager.Get(key, val)) {   // MemTable 未命中，查 SSTable
        std::cout << val << " (from SSTable)\n";
      } else {
        std::cout << "NOT FOUND!\n";
      }
    }

      // --- 范围扫描（当前仅覆盖 MemTable）---
    else if (cmd == "scan") {
      std::string start, end;
      std::cin >> start >> end;
      db.RangeQuery(start, end, [](const std::string &k, const std::string &v) {
        std::cout << k << " : " << v << "\n";
      });
      std::cout << "(Note: scan only covers memtable, not SSTables)\n";
    }

      // --- 强制 Flush ---
    else if (cmd == "flush") {
      if (in_batch) {
        std::cout << "Cannot flush while in batch. Commit or rollback first.\n";
        continue;
      }
      if (FlushMemTableToSSTable(db, wal, g_sst_dir, g_sst_manager)) {
        MaybeCompact();
        std::cout << "Flush successful.\n";
      } else {
        std::cout << "Flush failed.\n";
      }
    }

      // --- 保存（flush 的别名）---
    else if (cmd == "save") {
      if (in_batch) {
        std::cout << "Cannot save while in batch.\n";
        continue;
      }
      if (FlushMemTableToSSTable(db, wal, g_sst_dir, g_sst_manager)) {
        MaybeCompact();
        std::cout << "Saved (flushed) and WAL cleared.\n";
      } else {
        std::cerr << "Save failed.\n";
      }
    }

      // --- 退出系统 ---
    else if (cmd == "quit") {
      if (!in_batch) {
        // 退出前执行一次 Flush，确保所有数据持久化到 SSTable
        FlushMemTableToSSTable(db, wal, g_sst_dir, g_sst_manager);
      } else {
        std::cout << "Batch is active, rollback or commit before quit.\n";
        continue;
      }
      wal.close();
      std::cout << "Quit, data flushed to SSTables.\n";
      break;
    }

      // --- 帮助信息 ---
    else {
      std::cout << "Commands:\n"
                << "  put key value\n"
                << "  del key\n"
                << "  get key\n"
                << "  scan start_key end_key\n"
                << "  begin, commit, rollback\n"
                << "  flush (force memtable to SSTable)\n"
                << "  save (alias for flush)\n"
                << "  quit\n";
    }
  }

  return 0;
}