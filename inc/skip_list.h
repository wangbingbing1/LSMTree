//
// Created by wangbingbing on 2026/5/20.
//
// 基于跳表的内存键值存储（MemTable），支持批量操作和墓碑机制。

#ifndef TEST2_INCLUDE_SKIP_LIST_H_
#define TEST2_INCLUDE_SKIP_LIST_H_

#include <random>
#include <string>
#include <vector>
#include <functional>
#include <cassert>
#include <shared_mutex>
#include <mutex>
#include <tuple>
#include <memory>

// ======================== WriteBatch ========================
// 用于将多个写操作（PUT/DEL）打包成一个原子批次，
// 在提交时统一写入 WAL 并应用到跳表。
template<typename K, typename V>
class WriteBatch {
 public:
  enum kOpType { PUT, DEL };   // 操作类型

 private:
  struct Op {
    kOpType type;              // PUT 或 DEL
    K key;
    V value;                   // DEL 时忽略
    Op(kOpType t, const K &k, const V &v) : type(t), key(k), value(v) {}
  };

  std::vector<Op> ops_;        // 按添加顺序保存所有操作

 public:
  // 添加一条插入/更新操作
  void Put(const K &key, const V &value) {
    ops_.emplace_back(PUT, key, value);
  }

  // 添加一条删除操作（值用默认构造，无实际意义）
  void Delete(const K &key) {
    ops_.emplace_back(DEL, key, V());
  }

  // 清空批次（例如回滚时）
  void Clear() { ops_.clear(); }

  // 批次中的操作数量
  size_t Size() const { return ops_.size(); }

  // 只读访问所有操作，供 SkipList::ApplyBatch 使用
  const auto &GetOps() const { return ops_; }
};

// ======================== SkipList ========================
// 基于概率的跳表实现，用作内存表（MemTable）。
// 支持正常的键值存储、墓碑标记、范围查询及批量操作。
template<typename K, typename V>
class SkipList {
 private:
  struct Node {
    K key;
    V value;
    bool is_tombstone;                // true 表示该键已被删除（墓碑）
    std::vector<Node*> forward;       // 各层后继指针，索引0为最底层

    // 构造函数：初始化键、值、层数和墓碑状态
    Node(const K &k, const V &v, int level, bool tombstone = false);
  };

 public:
  SkipList();
  ~SkipList();

  // 插入/更新键值对（普通数据）
  void Insert(const K &k, const V &v);

  // 查询键的值，若键不存在或为墓碑则返回 false
  bool Get(const K &k, V &v) const;

  // 删除键：插入一个墓碑节点（不物理删除），保证幂等
  bool Remove(const K &k);

  // 返回当前跳表中的节点总数（含墓碑）
  size_t Size() const;

  // 清空所有节点（保留头节点）
  void Clear();

  // 批量应用 WriteBatch 中的操作，并先写 WAL
  void ApplyBatch(const WriteBatch<K, V> &batch, std::ostream &wal);

  // 遍历所有非墓碑节点，按 key 升序，调用 func
  void ForEach(std::function<void(const K &, const V &)> func) const {
    std::shared_lock lock(mutex_);
    Node* current = head_->forward[0];
    while (current) {
      if (!current->is_tombstone)      // 跳过墓碑
        func(current->key, current->value);
      current = current->forward[0];
    }
  }

  // 范围查询 [start, end]，仅输出非墓碑节点
  void RangeQuery(const K &start, const K &end,
                  std::function<void(const K &, const V &)> func) const {
    std::shared_lock lock(mutex_);
    Node* current = head_;
    // 定位到第一个 >= start 的节点
    for (int i = max_level_; i >= 0; --i) {
      while (current->forward[i] && current->forward[i]->key < start) {
        current = current->forward[i];
      }
    }
    current = current->forward[0];
    while (current && current->key <= end) {
      if (!current->is_tombstone)
        func(current->key, current->value);
      current = current->forward[0];
    }
  }

  // 遍历所有节点（含墓碑），回调接收 key, value, is_tombstone
  void ForEachWithTombstone(std::function<void(const K &, const V &, bool)> func) const;

  // 判断跳表中是否存在某个键（无论是否为墓碑）
  bool Contains(const K &key) const;

  class Iterator {
   public:
    // 构造函数：获取读锁并保存起始节点
    Iterator(const SkipList* list, Node* start) : list_(list),
                                                  current_(start),
                                                  lock_(std::make_unique<std::shared_lock<std::shared_mutex>>(list_->mutex_)) {};
    // 如果起始节点为空，且锁已持有，仍然有效（但 Valid() 会返回 false）

    // 转移锁的所有权
    Iterator(Iterator &&other) noexcept: list_(other.list_),
                                         current_(other.current_),
                                         lock_(std::move(other.lock_)) {
      other.current_ = nullptr;
    }

    // 禁止拷贝
    Iterator(const Iterator &) = delete;
    Iterator &operator=(const Iterator &) = delete;

    // 移动语义
    Iterator &operator=(Iterator &&other) noexcept {
      if(this!= &other){
        list_ = other.list_;
        current_ = other.current_;
        lock_ = std::move(other.lock_);
        other.current_ = nullptr;
      }
      return *this;
    }

    bool Valid() const { return current_ != nullptr; }
    void Next() { if (current_)current_ = current_->forward[0]; }
    const K &Key() const { return current_->key; }
    const V &Value() const { return current_->value; }
    bool IsTombstone() const { return current_->is_tombstone; }
   private:
    const SkipList* list_;
    Node* current_;
    std::unique_ptr<std::shared_lock<std::shared_mutex>> lock_;
  };

  Iterator GetIterator() const {
    return Iterator(this,head_->forward[0]);
  }

 private:
  Node* head_;                              // 头节点（哨兵，不存有效数据）
  int max_level_;                           // 当前最大层数
  float probability_;                       // 晋升概率（默认 0.5）
  size_t size_;                             // 节点总数（含墓碑）
  mutable std::shared_mutex mutex_;         // 读写锁：读共享，写独占
  mutable std::mt19937 rng_;                // 随机数生成器

  int RandomLevel();                        // 随机生成新节点的层数
  void MaybeIncreaseLevel();                // 必要时增加最大层数

  // 内部不加锁的插入，支持设置墓碑标记
  void InsertUnlocked(const K &key, const V &value, bool tombstone);

  // 内部不加锁的物理删除（已废弃）
  bool RemoveUnlocked(const K &key);
};

// ======================== 实现 ========================

// 检查键是否存在（包含墓碑）
template<typename K, typename V>
bool SkipList<K, V>::Contains(const K &key) const {
  std::shared_lock lock(mutex_);
  Node* current = head_;
  for (int i = max_level_; i >= 0; --i)
    while (current->forward[i] && current->forward[i]->key < key)
      current = current->forward[i];
  current = current->forward[0];
  return (current && current->key == key);
}

// 遍历所有节点（含墓碑）
template<typename K, typename V>
void SkipList<K, V>::ForEachWithTombstone(
    std::function<void(const K &, const V &, bool)> func) const {
  std::shared_lock lock(mutex_);
  Node* current = head_->forward[0];
  while (current) {
    func(current->key, current->value, current->is_tombstone);
    current = current->forward[0];
  }
}

// 内部不加锁插入
template<typename K, typename V>
void SkipList<K, V>::InsertUnlocked(const K &key, const V &value, bool tombstone) {
  std::vector<Node*> update(max_level_ + 1, nullptr);
  Node* current = head_;

  // 自顶向下寻找插入位置，记录每层的前驱
  for (int i = max_level_; i >= 0; i--) {
    while (current->forward[i] && current->forward[i]->key < key) {
      current = current->forward[i];
    }
    update[i] = current;
  }

  current = current->forward[0];

  // 键已存在：更新值和墓碑标记
  if (current && current->key == key) {
    current->value = value;
    current->is_tombstone = tombstone;
    return;
  }

  // 新节点：随机生成层数并插入
  int new_level = RandomLevel();
  Node* new_node = new Node(key, value, new_level, tombstone);

  for (int i = 0; i <= new_level; ++i) {
    new_node->forward[i] = update[i]->forward[i];
    update[i]->forward[i] = new_node;
  }
  size_++;
  MaybeIncreaseLevel();
}

// 物理删除节点（已废弃）
template<typename K, typename V>
bool SkipList<K, V>::RemoveUnlocked(const K &key) {
  std::vector<Node*> update(max_level_ + 1, nullptr);
  Node* current = head_;
  for (int i = max_level_; i >= 0; --i) {
    while (current->forward[i] && current->forward[i]->key < key) {
      current = current->forward[i];
    }
    update[i] = current;
  }
  current = current->forward[0];
  if (!current || current->key != key) return false;

  for (int i = 0; i <= max_level_; ++i) {
    if (update[i]->forward[i] != current) break;
    update[i]->forward[i] = current->forward[i];
  }
  delete current;
  size_--;
  return true;
}

// 批量应用：先写 WAL，再应用到跳表
template<typename K, typename V>
void SkipList<K, V>::ApplyBatch(const WriteBatch<K, V> &batch, std::ostream &wal) {
  // 将批次中所有操作写入 WAL
  for (const auto &op : batch.GetOps()) {
    if (op.type == WriteBatch<K, V>::PUT) {
      wal << "PUT " << op.key << " " << op.value << "\n";
    } else {
      wal << "DEL " << op.key << "\n";
    }
  }
  wal.flush();   // 强制落盘

  // 加锁后应用到内存表
  std::unique_lock lock(mutex_);
  for (const auto &op : batch.GetOps()) {
    if (op.type == WriteBatch<K, V>::PUT) {
      InsertUnlocked(op.key, op.value, false);
    } else {
      InsertUnlocked(op.key, V(), true);   // 插入墓碑
    }
  }
}

// 动态增加跳表最大层数（节点数超过阈值时）
template<typename K, typename V>
void SkipList<K, V>::MaybeIncreaseLevel() {
  size_t threshold = 1ULL << (max_level_ + 1);   // 2^(max_level_+1)
  if (size_ >= threshold && max_level_ < 32) {
    max_level_++;
    head_->forward.resize(max_level_ + 1, nullptr);
  }
}

// 节点构造函数
template<typename K, typename V>
SkipList<K, V>::Node::Node(const K &k, const V &v, int level, bool tombstone)
    : key(k), value(v), is_tombstone(tombstone), forward(level + 1, nullptr) {}

// SkipList 构造函数
template<typename K, typename V>
SkipList<K, V>::SkipList()
    : max_level_(16), probability_(0.5), size_(0), rng_(std::random_device{}()) {
  head_ = new Node(K(), V(), max_level_);
}

// 析构：释放所有节点
template<typename K, typename V>
SkipList<K, V>::~SkipList() {
  Clear();
  delete head_;
}

// 随机层数生成
template<typename K, typename V>
int SkipList<K, V>::RandomLevel() {
  int lvl = 0;
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  while (lvl < max_level_ && dist(rng_) < probability_) {
    lvl++;
  }
  return lvl;
}

// 公开的 Insert：普通写入
template<typename K, typename V>
void SkipList<K, V>::Insert(const K &key, const V &value) {
  std::unique_lock lock(mutex_);
  InsertUnlocked(key, value, false);
}

// 公开的 Get：忽略墓碑，仅对有效键返回值
template<typename K, typename V>
bool SkipList<K, V>::Get(const K &key, V &value) const {
  std::shared_lock lock(mutex_);
  Node* current = head_;
  for (int i = max_level_; i >= 0; --i) {
    while (current->forward[i] && current->forward[i]->key < key) {
      current = current->forward[i];
    }
  }
  current = current->forward[0];
  if (current && current->key == key) {
    if (!current->is_tombstone) {
      value = current->value;
      return true;
    }
  }
  return false;
}

// 公开的 Remove：插入墓碑，保证幂等
template<typename K, typename V>
bool SkipList<K, V>::Remove(const K &key) {
  std::unique_lock lock(mutex_);
  Node* current = head_;
  for (int i = max_level_; i >= 0; --i) {
    while (current->forward[i] && current->forward[i]->key < key) {
      current = current->forward[i];
    }
  }
  current = current->forward[0];
  // 如果已经是墓碑，直接返回成功
  if (current && current->key == key && current->is_tombstone) {
    return true;
  }
  // 插入墓碑（若键存在则更新标记，否则新建）
  InsertUnlocked(key, V(), true);
  return true;
}

template<typename K, typename V>
size_t SkipList<K, V>::Size() const {
  std::shared_lock lock(mutex_);
  return size_;
}

template<typename K, typename V>
void SkipList<K, V>::Clear() {
  std::unique_lock lock(mutex_);
  Node* current = head_->forward[0];
  while (current) {
    Node* next = current->forward[0];
    delete current;
    current = next;
  }
  for (int i = 0; i <= max_level_; ++i) {
    head_->forward[i] = nullptr;
  }
  size_ = 0;
}

#endif //TEST2_INCLUDE_SKIP_LIST_H_