//
// Created by wangbingbing on 2026/5/24.
//

#ifndef CVSTORE_INC_BLOOM_FILTER_H_
#define CVSTORE_INC_BLOOM_FILTER_H_

#include <vector>
#include <string>
#include <cstdint>
#include <functional>
#include <cmath>

// K 键的类型，要求支持 std::hash<K>
template<typename K>
class BloomFilter {
 public:
  /**
   * 构造函数，根据预期键数量和每位键的比特数计算位数组大小及哈希函数个数。
   *
   * @param bits_per_key   每个键平均分配的比特数
   * @param expected_keys  预期插入的键数量
   */
  BloomFilter(int bits_per_key, size_t expected_keys) {
    // 计算位数组总长度 至少 64 位
    size_t bits = bits_per_key * expected_keys;
    if (bits < 64) bits = 64;
    bits_.resize(bits);

    if (expected_keys == 0) {
      bits_.resize(64);
      k_ = 1;
      return;
    }
    // 根据推导出的最优哈希函数个数公式：
    // k = (bits / n) * ln(2) ≈ ratio * 0.693
    double ratio = static_cast<double>(bits) / expected_keys;
    k_ = static_cast<int>(std::log(2.0) * ratio);
    if (k_ < 1) k_ = 1;       // 至少使用 1 个哈希函数
    if (k_ > 30) k_ = 30;     // 最大 30 个
  }

  void Add(const K &key) {
    auto h = hash(key);
    size_t h1 = h.first;
    size_t h2 = h.second;
    for (int i = 0; i < k_; ++i) {
      size_t idx = (h1 + i * h2) % bits_.size();
      bits_[idx] = true;
    }
  }

  bool MayContain(const K &key) const {
    auto h = hash(key);
    size_t h1 = h.first;
    size_t h2 = h.second;
    for (int i = 0; i < k_; ++i) {
      size_t idx = (h1 + i * h2) % bits_.size();
      if (!bits_[idx]) return false;
    }
    return true;
  }

/**
 * 将布隆过滤器序列化为二进制字符串，方便持久化存储或网络传输。
 * 序列化格式：[k_(4字节)] [bit_len(8字节)] [位数组紧凑字节序列]
 *   - k_ : 哈希函数个数 (int, 通常4字节)
 *   - bit_len : 位数组总长度 (size_t, 8字节)
 *   - 位数组 : 每 8 位压缩为 1 字节，共 ((bit_len + 7) / 8) 字节
 */
  std::string Serialize() const {
    std::string data;

    // 1. 写入哈希函数个数 k_
    // reinterpret_cast 将 &k_ 解释为 char*，以便逐字节写入
    data.append(reinterpret_cast<const char*>(&k_), sizeof(k_));

    // 2. 获取位数组长度并写入
    size_t bit_len = bits_.size();
    data.append(reinterpret_cast<const char*>(&bit_len), sizeof(bit_len));

    // 3. 将 vector<bool> 压缩为紧凑的字节数组
    // 计算所需字节数：bit_len / 8 向上取整
    size_t byte_len = (bit_len + 7) / 8;
    std::vector<char> bytes(byte_len, 0);   // 初始全0

    // 遍历所有位，将 true 对应的位置位
    for (size_t i = 0; i < bit_len; ++i) {
      if (bits_[i]) {
        // 确定该位属于哪个字节 (i/8) 以及字节内的偏移 (i%8)
        bytes[i / 8] |= (1 << (i % 8));
      }
    }

    // 4. 追加压缩后的位数组到序列化数据
    data.append(bytes.data(), byte_len);

    return data;
  }

/**
 * 从二进制字节串反序列化布隆过滤器。
 * 序列化格式：[k_ (int)] [bit_len (size_t)] [压缩位数组 (byte_len 字节)]
 *
 * @param data 由 Serialize() 生成的紧凑字节序列
 * @return 复原的 BloomFilter 对象。若数据不合法，返回默认过滤器（10 bits/key, 1000键）
 */
  static BloomFilter Deserialize(const std::string &data) {
    // 头部至少需要存放一个 int 和一个 size_t
    if (data.size() < sizeof(int) + sizeof(size_t)) {
      return BloomFilter(10, 1000);   // 数据过短，返回安全默认值
    }

    const char* ptr = data.data();

    // 1. 读取哈希函数个数 k_
    int k = *reinterpret_cast<const int*>(ptr);
    ptr += sizeof(int);

    // 2. 读取位数组长度 bit_len
    size_t bit_len = *reinterpret_cast<const size_t*>(ptr);
    ptr += sizeof(size_t);

    // 3. 计算压缩后的位数组字节数（向上取整）
    size_t byte_len = (bit_len + 7) / 8;

    // 4. 验证数据总长度是否完全匹配
    if (data.size() != sizeof(int) + sizeof(size_t) + byte_len) {
      return BloomFilter(10, 1000);   // 长度不一致，数据损坏，返回默认
    }

    BloomFilter filter(10, 1000);   // 占位构造
    filter.k_ = k;                  // 覆盖为正确的 k_
    if (filter.k_ < 1) filter.k_ = 1;
    if (filter.k_ > 30) filter.k_ = 30;

    filter.bits_.resize(bit_len);   // 调整位数组为正确长度

    // 5. 逐位还原 bits_ 的内容
    for (size_t i = 0; i < bit_len; ++i) {
      // 找到第 i 位所在的字节，用掩码提取对应位
      bool bit = (ptr[i / 8] & (1 << (i % 8))) != 0;
      filter.bits_[i] = bit;
    }

    return filter;
  }

  size_t Size() const { return bits_.size(); }

 private:
  /**
   * 生成两个独立的哈希值
   *   h(i, key) = h1 + i * h2  （i = 0 .. k-1）
   * @param key  待哈希的键
   * @return     包含两个 size_t 哈希值的 pair (h1, h2)
   */
  std::pair<size_t, size_t> hash(const K &key) const {
    std::hash<K> hasher;
    size_t h1 = hasher(key);                       // 第一个哈希值
    // 第二个哈希值通过一个固定扰动产生，增加独立性
    size_t h2 = hasher(key) + 0x9e3779b97f4a7c15ULL;
    return {h1, h2};
  }

  std::vector<bool> bits_;   // 位数组，true 表示对应位置被置 1
  int k_;                    // 哈希函数个数（k 个）
};

#endif //CVSTORE_INC_BLOOM_FILTER_H_