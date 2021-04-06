#include "fs.hpp"
#include <fstream>
#include <sstream>

namespace frill {
inline std::ifstream open_file(const fs::path &file_path,
                               std::ios::openmode mode) {
  std::ifstream t(file_path, mode);
  if (!t.good()) {
    std::stringstream ss;
    ss << "failed to open " << file_path;
    throw std::runtime_error(ss.str());
  }
  return t;
}

std::string read_file_str(const fs::path &file_path) {
  auto t = open_file(file_path, std::ios::in);
  std::string str((std::istreambuf_iterator<char>(t)),
                  std::istreambuf_iterator<char>());
  return str;
}

std::vector<uint8_t> read_file_binary(const fs::path &file_path) {
  auto t = open_file(file_path, std::ios::in | std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(t)),
                            std::istreambuf_iterator<char>());
  return data;
}

void write_file(const fs::path &file_path, const char *data, size_t size) {
  auto dir = file_path.parent_path();
  fs::create_directories(dir);

  std::ofstream os(file_path, std::ios::out | std::ios::binary);
  if (!os.good()) {
    std::stringstream ss;
    ss << "failed to open " << file_path;
    throw std::runtime_error(ss.str());
  }
  os.write(data, size);
  os.close();
}

void write_file(const fs::path &file_path, const std::string &str) {
  write_file(file_path, str.c_str(), str.size());
}
} // namespace frill
