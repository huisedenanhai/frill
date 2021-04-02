#pragma once

#include <filesystem>
#include <json/json.hpp>
#include <set>
#include <string>

namespace frill {
namespace fs = std::filesystem;
namespace json = nlohmann;

struct TargetId {
  fs::path path;
  std::set<std::string> flags;

  json::json to_json() const;

  void load_json(const json::json &js);

  bool operator==(const TargetId &rhs) const;

  bool operator!=(const TargetId &rhs) const;
};

} // namespace frill