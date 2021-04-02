#pragma once

#include <string>
#include <vector>

namespace frill {
struct Shader {
  std::vector<uint8_t> code;
};

class IArchive {
public:
  virtual Shader
  load(const char *path, const char **flags, uint32_t flag_count) = 0;
};

class FolderArchive : public IArchive {
public:
  Shader
  load(const char *path, const char **flags, uint32_t flag_count) override;
};
} // namespace frill