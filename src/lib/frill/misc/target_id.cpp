#include "target_id.hpp"

namespace frill {
json::json TargetId::to_json() const {
  json::json js{};
  js["path"] = path.string();
  auto flags_js = json::json::array();
  for (auto &flag : flags) {
    flags_js.push_back(flag);
  }
  js["flags"] = flags_js;
  return js;
}

void TargetId::load_json(const nlohmann::json &js) {
  path = js["path"].get<std::string>();
  flags.clear();
  for (auto &flag : js["flags"]) {
    flags.insert(flag.get<std::string>());
  }
}

bool TargetId::operator==(const TargetId &rhs) const {
  return path == rhs.path && flags == rhs.flags;
}

bool TargetId::operator!=(const TargetId &rhs) const {
  return !(*this == rhs);
}
} // namespace frill