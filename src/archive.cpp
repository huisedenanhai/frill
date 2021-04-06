#include "archive.hpp"
#include "misc/fs.hpp"
#include "misc/target_id.hpp"
#include <json/json.hpp>

namespace json = nlohmann;

namespace frill {
template <typename It>
TargetId make_target_id(const fs::path &path, It flag_start, It flag_end) {
  TargetId id;
  id.path = path;
  id.flags.insert(flag_start, flag_end);
  return id;
}

class FolderArchive::Impl {
public:
  explicit Impl(fs::path folder_path) : _folder_path(std::move(folder_path)) {
    auto index_json = read_file_str(_folder_path / "index.js");
    auto js = json::json::parse(index_json);
    for (auto &term : js) {
      TargetId id;
      id.load_json(term["target"]);
      auto term_path = fs::canonical(fs::absolute(
          _folder_path / (term["uid"].get<std::string>() + ".spv")));
      _index.emplace(std::move(id), std::move(term_path));
    }
  }

  std::optional<Shader> load(const TargetId &id) {
    Shader shader;
    auto it = _index.find(id);
    if (it == _index.end()) {
      return std::nullopt;
    }
    try {
      shader.code = read_file_binary(it->second);
    } catch (std::exception &e) {
      return std::nullopt;
    }
    return shader;
  }

private:
  fs::path _folder_path;
  std::unordered_map<TargetId, fs::path> _index;
};

FolderArchive::FolderArchive(const char *folder_path)
    : _impl(std::make_unique<Impl>(folder_path)) {}

std::optional<Shader>
FolderArchive::load(const char *path, const char **flags, uint32_t flag_count) {
  auto id = make_target_id(path, flags, flags + flag_count);
  return _impl->load(id);
}
} // namespace frill