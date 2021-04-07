#pragma once

#include "fs.hpp"
#include <filesystem>
#include <json/json.hpp>
#include <set>
#include <string>

namespace frill {
namespace json = nlohmann;

struct TargetId {
  fs::path path;
  std::set<std::string> flags;

  json::json to_json() const;

  void load_json(const json::json &js);

  bool operator==(const TargetId &rhs) const;

  bool operator!=(const TargetId &rhs) const;
};

template <typename T>
inline void hash_combine(std::size_t &seed, const T &val) {
  std::hash<T> hasher;
  seed ^= hasher(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

} // namespace frill

namespace std {
template <> struct hash<frill::TargetId> {
  size_t operator()(const frill::TargetId &id) const {
    size_t seed = 0;
    frill::hash_combine(seed, id.path.string());
    for (auto &flag : id.flags) {
      frill::hash_combine(seed, flag);
    }
    return seed;
  }
};
} // namespace std
