#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace frill {
namespace fs {
// automatically convert utf-8 string to target platform path codec
class path : public std::filesystem::path {
public:
  path() = default;
  path(const char *str);
  path(const std::string &str) : path(str.c_str()) {}
  path(std::filesystem::path p) : std::filesystem::path(std::move(p)) {}
  path(const path &) = default;
  path(path &&) = default;

  path &operator=(const path &) = default;
  path &operator=(path &&) = default;

  std::string string() const;
};

inline path absolute(const path &p) {
  return std::filesystem::absolute(p);
}

inline path canonical(const path &p) {
  return std::filesystem::canonical(p);
}

inline bool create_directories(const path &p) {
  return std::filesystem::create_directories(p);
}

inline bool exists(const path &p) {
  return std::filesystem::exists(p);
}

using std::filesystem::file_time_type;
inline file_time_type last_write_time(const path &p) {
  return std::filesystem::last_write_time(p);
}

inline path relative(const path &p,
                     const path &base = std::filesystem::current_path()) {
  return std::filesystem::relative(p, base);
}
} // namespace fs

std::string read_file_str(const fs::path &file_path);

std::vector<uint8_t> read_file_binary(const fs::path &file_path);

void write_file(const fs::path &file_path, const char *data, size_t size);

void write_file(const fs::path &file_path, const std::string &str);
} // namespace frill