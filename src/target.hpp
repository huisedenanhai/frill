#pragma once

#include <frill/misc/fs.hpp>
#include <frill/misc/target_id.hpp>
#include <set>

namespace fs = frill::fs;

struct Target {
  fs::path relative_path; // relative to input directory
  frill::TargetId id{};   // using absolute path
  std::set<fs::path> include_dirs;
  fs::path declaring_config_file; // absolute path
  std::string uid;

  std::string display() const;

  fs::path output_file_relative_dir(const std::string &ext = ".spv") const;

  bool operator==(const Target &rhs) const {
    return id == rhs.id;
  }

  bool check_outdated(const fs::path &cache_path) const;

  // return dependencies
  void compile(const fs::path &output_dir, const fs::path &cache_path) const;

private:
  fs::path get_time_stamp_path(const fs::path &cache_path) const;
};

void raise_error(const fs::path &config, const std::string &err);
