#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace frill {
namespace fs = std::filesystem;

std::string read_file_str(const fs::path &file_path);

std::vector<uint8_t> read_file_binary(const fs::path &file_path);

void write_file(const fs::path &file_path, const char *data, size_t size);

void write_file(const fs::path &file_path, const std::string &str);
} // namespace frill