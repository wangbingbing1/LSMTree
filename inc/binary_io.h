//
// Created by wangbingbing on 2026/5/26.
//

#ifndef CVSTORE_INC_BINARY_IO_H_
#define CVSTORE_INC_BINARY_IO_H_

#include <iostream>
#include <string>
#include <type_traits>
#include <cstdint>
#include <sstream>

// 通用 POD 类型写入（排除 std::string）
template<typename T>
typename std::enable_if<std::is_pod<T>::value && !std::is_same<T, std::string>::value>::type
WriteBinary(std::ostream &os, const T &value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

// std::string 特化写入：先写 4 字节长度，再写字符数据
inline void WriteBinary(std::ostream &os, const std::string &str) {
  uint32_t len = static_cast<uint32_t>(str.size());
  WriteBinary(os, len);          // 这里会调用 POD 版本（uint32_t 是 POD）
  os.write(str.data(), len);
}

// 通用 POD 类型读取
template<typename T>
typename std::enable_if<std::is_pod<T>::value>::type
ReadBinary(std::istream &is, T &value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

// std::string 特化读取
inline void ReadBinary(std::istream &is, std::string &str) {
  uint32_t len;
  ReadBinary(is, len);
  str.resize(len);
  is.read(&str[0], len);
}

// 写入墓碑标记
inline void WriteFlags(std::ostream &os, bool is_tombstone) {
  uint8_t flags = is_tombstone ? 1 : 0;
  WriteBinary(os, flags);
}

inline bool ReadFlags(std::istream &is) {
  uint8_t flags;
  ReadBinary(is, flags);
  return (flags & 1) != 0;
}

#endif //CVSTORE_INC_BINARY_IO_H_