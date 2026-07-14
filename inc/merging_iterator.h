//
// Created by wangbingbing on 2026/5/26.
//

#ifndef CVSTORE_INC_MERGING_ITERATOR_H_
#define CVSTORE_INC_MERGING_ITERATOR_H_

#include <vector>
#include <memory>
#include <queue>
#include <functional>

/**
 * 统一的迭代器接口
 */
template<typename K, typename V>
class IteratorInterface {
 public:
  virtual ~IteratorInterface() = default;
  virtual bool Valid() const = 0;
  virtual void Next() = 0;
  virtual const K &Key() const = 0;
  virtual const V &Value() const = 0;
  virtual bool IsTombstone() const = 0;
};

/**
 * 适配 SkipListIterator 的包装
 */

template<typename K, typename V>
class SkipListIteratorWrapper : public IteratorInterface<K, V> {
 public:
  explicit SkipListIteratorWrapper(typename SkipList<K, V>::Iterator&& it) : it_(std::move(it)) {}
  bool Valid() const override { return it_.Valid(); }
  void Next() override { it_.Next(); }
  const K &Key() const override { return it_.Key(); }
  const V &Value() const override { return it_.Value(); }
  bool IsTombstone() const override { return it_.IsTombstone(); }
 private:
  typename SkipList<K, V>::Iterator it_;
};

/**
 * 适配 SSTableIterator 的包装
 */
template<typename K, typename V>
class SSTableIteratorWrapper : public IteratorInterface<K, V> {
 public:
  explicit SSTableIteratorWrapper(typename SSTable<K, V>::Iterator&& it) : it_(std::move(it)) {}
  bool Valid() const override { return it_.Valid(); }
  void Next() override { it_.Next(); }
  const K &Key() const override { return it_.Key(); }
  const V &Value() const override { return it_.Value(); }
  bool IsTombstone() const override { return it_.IsTombstone(); }
 private:
  typename SSTable<K, V>::Iterator it_;
};

/**
 * 多路归并迭代器：将多个有序子迭代器合并成一个全局有序的迭代器。
 * 处理相同键的版本去重（保留最新）及墓碑跳过。
 *
 * @tparam K  键类型
 * @tparam V  值类型
 */
template<typename K, typename V>
class MergingIterator : public IteratorInterface<K, V> {
 public:
  // 实现 IteratorInterface 的只读访问方法
  const K &Key() const override { return current_key_; }
  const V &Value() const override { return current_value_; }
  bool IsTombstone() const override { return current_tombstone_; }
  bool Valid() const override { return valid_; }

  /**
   * 移动到下一个有效键值对（跳过墓碑和旧版本）。
   * 每次调用会确保当前键为最新的非墓碑值；若为墓碑则自动跳过。
   */
  void Next() override {
    while (!heap_.empty()) {
      // 弹出堆顶（当前最小的键）
      auto top = heap_.top();
      heap_.pop();
      size_t idx = top.idx;
      current_key_ = top.key;
      current_value_ = iters_[idx]->Value();
      current_tombstone_ = iters_[idx]->IsTombstone();

      // 推进该迭代器
      iters_[idx]->Next();
      if (iters_[idx]->Valid()) {
        heap_.push({idx, iters_[idx]->Key()});
      }

      // 跳过堆中所有与当前键相同的旧版本（其他文件中的相同键）
      while (!heap_.empty() && heap_.top().key == current_key_) {
        auto &dup = heap_.top();
        iters_[dup.idx]->Next();
        heap_.pop();
        if (iters_[dup.idx]->Valid()) {
          heap_.push({dup.idx, iters_[dup.idx]->Key()});
        }
      }

      // 如果当前键是墓碑，则继续循环找下一个键
      if (current_tombstone_) {
        continue;
      }

      // 找到有效键，退出循环
      valid_ = true;
      return;
    }
    valid_ = false;
  }

  /**
   * 构造函数：接收一组子迭代器，初始化堆并定位到第一个有效元素。
   * @param iters  子迭代器的 unique_ptr 向量（所有权转移）
   */
  explicit MergingIterator(std::vector<std::unique_ptr<IteratorInterface<K, V>>> iters)
      : iters_(std::move(iters)), valid_(false) {
    // 将所有有效的子迭代器的当前键压入最小堆
    for (size_t i = 0; i < iters_.size(); ++i) {
      if (iters_[i]->Valid()) {
        heap_.push({i, iters_[i]->Key()});
      }
    }
    Next();   // 初始化到第一个有效元素
  }

 private:
  /// 堆元素：记录键及其来源迭代器的索引
  struct HeapItem {
    size_t idx;   // 子迭代器在 iters_ 中的索引
    K key;        // 该迭代器当前指向的键
    bool operator>(const HeapItem &other) const { return key > other.key; }
  };

  /// 比较器：用于 priority_queue 构建最小堆
  struct Compare {
    bool operator()(const HeapItem &a, const HeapItem &b) const {
      if (a.key != b.key)
        return a.key > b.key;
      return a.idx > b.idx; // key 相同时，索引大的（新数据）优先级高
    }
  };

  /// 所有子迭代器（按原始顺序存储，索引即对应堆中的 idx）
  std::vector<std::unique_ptr<IteratorInterface<K, V>>> iters_;

  /// 最小堆，按 key 排序，堆顶为当前全局最小键
  std::priority_queue<HeapItem, std::vector<HeapItem>, Compare> heap_;

  bool valid_;               // 当前迭代器是否有效
  K current_key_;            // 当前键
  V current_value_;          // 当前值
  bool current_tombstone_;   // 当前键是否为墓碑
};
#endif //CVSTORE_INC_MERGING_ITERATOR_H_