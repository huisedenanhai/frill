#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frill {
struct Shader {
  std::vector<uint8_t> code;
};

class IArchive {
public:
  virtual std::optional<Shader>
  load(const char *path, const char **flags, uint32_t flag_count) = 0;

  template <size_t N>
  std::optional<Shader> load(const char *path,
                             const std::array<const char *, N> &flags) {
    return load(path, flags.data(), static_cast<uint32_t>(flags.size()));
  }
};

class FolderArchive : public IArchive {
public:
  explicit FolderArchive(const char *folder_path);

  std::optional<Shader>
  load(const char *path, const char **flags, uint32_t flag_count) override;

private:
  class Impl;
  std::unique_ptr<Impl> _impl;
};
} // namespace frill