//
// Created by wangbingbing on 2026/5/25.
//

#ifndef CVSTORE_INC_BLOCK_CACHE_H_
#define CVSTORE_INC_BLOCK_CACHE_H_

#include <string>
#include <unordered_map>
#include <list>
#include <mutex>

/**
 * 主要用于缓存数据块（如 SSTable 的数据块），减少磁盘 I/O。
 * @tparam V  缓存值的类型，需支持 std::is_pod 或提供特化的 EstimateSize。
 */
template<typename V>
class BlockCache {
 public:

  explicit BlockCache(size_t capacity_bytes) : capacity_bytes_(capacity_bytes), total_bytes_(0) {}

  /**
  * 插入或更新一个键值对，自动维护 LRU 顺序和总容量限制。
  *
  * @param key   缓存键
  * @param value 缓存值
  */
  void Insert(const std::string &key, const V &value) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t value_bytes = EstimateSize(value,std::is_same<V,std::string>{});
    auto it = cache_map_.find(key);

    // ----- 键已存在：更新值 + 移到头部 + 必要时淘汰其他条目 -----
    if (it != cache_map_.end()) {
      // 1. 调整总占用：减去旧值大小，加上新值大小
      total_bytes_ -= it->second.first.size_bytes;
      total_bytes_ += value_bytes;

      // 2. 更新缓存条目内容和大小
      it->second.first.value = value;
      it->second.first.size_bytes = value_bytes;

      // 3. 将该键移动到 LRU 链表头部，表示“最近使用”
      lru_list_.splice(lru_list_.begin(), lru_list_, it->second.second);

      // 4. 若更新后总占用超过容量，需要淘汰其他条目（不能淘汰刚更新的自身）
      //    lru_list_.size() > 1 保证不会在只有一个条目时错误地淘汰掉它。
      while (total_bytes_ > capacity_bytes_ && lru_list_.size() > 1) {
        Evict();  // 每次淘汰链表尾部（最久未使用）的一个条目
      }
      return;
    }

    // ----- 键不存在：先淘汰直到有足够空间，再插入新条目 -----
    while (total_bytes_ + value_bytes > capacity_bytes_ && !lru_list_.empty()) {
      Evict();
    }

    // 插入到链表头部（最新），并记录迭代器
    lru_list_.push_front(key);
    cache_map_[key] = {{value, value_bytes}, lru_list_.begin()};
    total_bytes_ += value_bytes;
  }

/**
 * 在缓存中查找键 key，如果找到，将对应的值写入 value，并将该键标记为“最近使用”。
 *
 * @param key    要查找的键
 * @param value  [out] 用于接收缓存值的引用
 * @return       如果键存在，返回 true，否则返回 false
 */
  bool Lookup(const std::string &key, V &value) {
    // 1. 加锁
    std::lock_guard<std::mutex> lock(mutex_);

    // 2. 在哈希表中查找键
    auto it = cache_map_.find(key);
    if (it == cache_map_.end()) {
      // 未命中，直接返回 false
      return false;
    }

    // 3. 命中：将该键移动到 LRU 链表的头部，表示“最近被访问”
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.second);

    // 4. 通过引用参数将缓存值传出
    value = it->second.first.value;

    // 5. 返回 true 表示命中
    return true;
  }

  size_t TotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_bytes_;
  }
 private:
  /// 单个缓存条目，保存实际数据及其占用的字节数
  struct CacheEntry {
    V value;               // 缓存的数据对象
    size_t size_bytes;     // 该对象占用的内存大小（用于容量控制）
  };

  size_t capacity_bytes_;    // 缓存总容量上限（字节）
  size_t total_bytes_;       // 当前缓存已使用的字节数

  /**
   * LRU 链表：存储键的插入/访问顺序。
   * 头部（front）是最近访问的键，尾部（back）是最久未访问的键。
   * 当需要淘汰时，从尾部弹出键。
   */
  std::list<std::string> lru_list_;

  /**
   * 哈希表：从键快速映射到缓存条目及其在 LRU 链表中的位置。
   * - first：CacheEntry（缓存数据）
   * - second：指向 lru_list_ 中对应键节点的迭代器，用于 O(1) 移动节点到链表头部
   */
  std::unordered_map<std::string,
                     std::pair<CacheEntry,
                               typename std::list<std::string>::iterator>> cache_map_;

  mutable std::mutex mutex_;   // 互斥锁，保证线程安全

  /**
   * 淘汰函数：从缓存中移除最久未使用的条目。
   * 从 LRU 链表尾部取出键，在哈希表中查找并删除，同时更新已使用字节数。
   */
  void Evict() {
    if (lru_list_.empty())
      return;

    // 获取并移除 LRU 链表尾部的键（最久未使用）
    std::string last_key = lru_list_.back();
    lru_list_.pop_back();

    auto it = cache_map_.find(last_key);
    if (it != cache_map_.end()) {
      total_bytes_ -= it->second.first.size_bytes;
      cache_map_.erase(it);                         // 从哈希表中删除条目
    }
  }

  // ----- 标签分发重载：用于计算值占用的字节数 -----
  // 处理 std::string（tag = true_type）
  size_t EstimateSize(const std::string& val, std::true_type) const {
    return val.size();
  }

  // 处理其他类型（tag = false_type）
  template<typename T>
  size_t EstimateSize(const T& val, std::false_type) const {
    (void)val;
    return sizeof(T);
  }
};


#endif //CVSTORE_INC_BLOCK_CACHE_H_