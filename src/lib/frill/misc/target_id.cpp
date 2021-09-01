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

json::json IndexTerm::to_json() const {
  json::json term;
  term["target"] = target.to_json();
  term["uid"] = uid;
  return term;
}

void IndexTerm::load_json(const nlohmann::json &js) {
  target.load_json(js["target"]);
  uid = js["uid"].get<std::string>();
}

json::json IndexFile::to_json() const {
  auto js = json::json::array();
  for (auto &t : targets) {
    js.push_back(t.to_json());
  }
  return js;
}

void IndexFile::load_json(const nlohmann::json &js) {
  targets.clear();
  for (auto &term_js : js) {
    IndexTerm term{};
    term.load_json(term_js);
    targets.push_back(std::move(term));
  }
}

} // namespace frill