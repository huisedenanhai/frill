#include "frill.h"
#include <string>
#include <unordered_map>

namespace frill {
struct DataRange {
  size_t offset;
  size_t len;
};

struct Index {
  Index() {
    //@INDEX
  }
  std::unordered_map<std::string, DataRange> indices{};
};

static uint8_t s_bytes[] = {
    //@BYTES
};

void get_asset_bytes(const char *uri, void **ptr, size_t *size) {
  static Index s_indices{};

  if (s_indices.indices.count(uri) == 0) {
    *ptr = 0;
    *size = 0;
    return;
  }

  DataRange range = s_indices.indices[uri];
  *ptr = s_bytes + range.offset;
  *size = range.len;
}
} // namespace frill