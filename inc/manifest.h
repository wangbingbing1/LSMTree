#ifndef CVSTORE_INC_MANIFEST_H_
#define CVSTORE_INC_MANIFEST_H_

#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

#include "binary_io.h"

namespace fs = std::filesystem;

namespace leveled {

/**
 * 描述单个 SSTable 文件在某层中的元信息。
 * 可用于记录其文件位置、大小、键范围及版本，以便在重启时恢复层级结构。
 */
template<typename K>
struct FileMeta {
  std::string filename;    // 仅基础文件名（不含路径）
  uint64_t file_size;      // 文件大小（字节）
  K first_key;             // 该文件的最小键
  K last_key;              // 该文件的最大键
  uint64_t version;        // 版本号（数字越大越新）

  /// 将元数据二进制序列化到输出流
  void Serialize(std::ostream &os) const {
    WriteBinary(os, filename);
    WriteBinary(os, file_size);
    WriteBinary(os, first_key);
    WriteBinary(os, last_key);
    WriteBinary(os, version);
  }

  /// 从输入流反序列化一个 FileMeta 对象
  static FileMeta Deserialize(std::istream &is) {
    FileMeta meta;
    ReadBinary(is, meta.filename);
    ReadBinary(is, meta.file_size);
    ReadBinary(is, meta.first_key);
    ReadBinary(is, meta.last_key);
    ReadBinary(is, meta.version);
    return meta;
  }
};

/**
 * Manifest 文件管理类，提供静态方法用于保存/加载所有层级的元数据。
 *
 * 文件命名规则：
 *   MANIFEST-<seq> ：二进制格式的清单文件，<seq> 递增。
 *   CURRENT        ：文本文件，记录当前活跃的 MANIFEST 文件名。
 *
 * 通过引入序号和 CURRENT 文件，保证崩溃后能恢复到最新的有效版本。
 */
class Manifest {
 public:
  /// 文件魔数，用于快速识别 MANIFEST 文件（"LSMMANIF"）
  static constexpr uint64_t MAGIC = 0x4C534D4D414E4946;
  /// 当前支持的清单文件格式版本
  static constexpr uint32_t CURRENT_VERSION = 1;

  /**
   * 保存所有层级的元数据到新的 MANIFEST 文件，并原子更新 CURRENT。
   *
   * @param dir     数据目录
   * @param levels  每个层级包含的 FileMeta 列表（levels[0] 为 L0）
   * @return 成功返回 true，否则 false
   */
  template<typename K>
  static bool Save(const std::string &dir,
                   const std::vector<std::vector<FileMeta<K>>> &levels) {
    fs::create_directories(dir);

    // 生成新的序列号（最大序列号 + 1）
    uint64_t new_seq = GetMaxManifestSeq(dir) + 1;
    std::string new_manifest = dir + "/MANIFEST-" + std::to_string(new_seq);

    std::ofstream out(new_manifest, std::ios::binary);
    if (!out)
      return false;

    // 写入魔数和版本号
    WriteBinary(out, MAGIC);
    WriteBinary(out, CURRENT_VERSION);

    // 写入总层数
    uint32_t level_cnt = static_cast<uint32_t>(levels.size());
    WriteBinary(out, level_cnt);

    // 逐层写入
    for (size_t lvl = 0; lvl < levels.size(); ++lvl) {
      uint32_t lvl_num = static_cast<uint32_t>(lvl);
      WriteBinary(out, lvl_num);

      uint32_t file_cnt = static_cast<uint32_t>(levels[lvl].size());
      WriteBinary(out, file_cnt);

      for (const auto &meta : levels[lvl]) {
        meta.Serialize(out);
      }
    }

    out.close();
    if (!out)
      return false;   // 写入过程中可能出错（磁盘满等）

    // 原子更新 CURRENT 文件（先写临时文件，再重命名）
    std::string current_tmp = dir + "/CURRENT.tmp";
    std::ofstream cur_out(current_tmp);
    if (!cur_out)
      return false;
    cur_out << "MANIFEST-" << new_seq << "\n";
    cur_out.close();

    fs::rename(current_tmp, dir + "/CURRENT");
    return true;
  }

  /**
   * 从目录加载最新的 MANIFEST，恢复层级结构和最后使用的序列号。
   *
   * @param dir       数据目录
   * @param levels    输出参数，恢复后的层级元数据
   * @param last_seq  输出参数，当前 MANIFEST 的序列号
   * @return 成功返回 true，否则 false
   */
  template<typename K>
  static bool Load(const std::string &dir,
                   std::vector<std::vector<FileMeta<K>>> &levels,
                   uint64_t &last_seq) {
    // 1. 从 CURRENT 文件获取当前 MANIFEST 的序列号
    last_seq = GetCurrentSeq(dir);
    if (last_seq == 0)
      return false;

    // 2. 构造完整路径
    std::string manifest_file = dir + "/MANIFEST-" + std::to_string(last_seq);
    std::ifstream in(manifest_file, std::ios::binary);
    if (!in)
      return false;

    // 3. 校验魔数
    uint64_t magic;
    ReadBinary(in, magic);
    if (magic != MAGIC)
      return false;

    // 4. 校验版本号
    uint32_t version;
    ReadBinary(in, version);
    if (version != CURRENT_VERSION)
      return false;

    // 5. 读取层级数量并分配空间
    uint32_t level_cnt;
    ReadBinary(in, level_cnt);
    levels.clear();
    levels.resize(level_cnt);

    // 6. 逐层读取元数据
    for (uint32_t i = 0; i < level_cnt; ++i) {
      uint32_t lvl_num;
      ReadBinary(in, lvl_num);
      if (lvl_num >= level_cnt)
        return false;   // 层级号非法

      uint32_t file_cnt;
      ReadBinary(in, file_cnt);
      levels[lvl_num].reserve(file_cnt);
      for (uint32_t j = 0; j < file_cnt; ++j) {
        levels[lvl_num].push_back(FileMeta<K>::Deserialize(in));
      }
    }
    return true;
  }

 private:
  /// 获取目录中最大的 MANIFEST 序列号
  static uint64_t GetMaxManifestSeq(const std::string &dir) {
    uint64_t max_seq = 0;
    if (!fs::exists(dir))
      return 0;

    for (const auto &entry : fs::directory_iterator(dir)) {
      std::string name = entry.path().filename().string();
      if (name.rfind("MANIFEST-", 0) == 0) {   // 以 "MANIFEST-" 开头
        std::string num_str = name.substr(9); // 提取数字部分
        try {
          uint64_t seq = std::stoull(num_str);
          if (seq > max_seq) max_seq = seq;
        } catch (...) {}
      }
    }
    return max_seq;
  }

  /// 读取 CURRENT 文件，获取当前使用的 MANIFEST 序列号
  static uint64_t GetCurrentSeq(const std::string &dir) {
    std::string current_path = dir + "/CURRENT";
    std::ifstream cur(current_path);
    if (!cur.is_open())
      return 0;

    std::string manifest_name;
    std::getline(cur, manifest_name);
    if (manifest_name.rfind("MANIFEST", 0) != 0)
      return 0; // 格式不符

    std::string num_str = manifest_name.substr(9); // "MANIFEST-" 后即为数字
    try {
      return std::stoull(num_str);
    } catch (...) {
      return 0;
    }
  }
};

} // namespace leveled

#endif //CVSTORE_INC_MANIFEST_H_