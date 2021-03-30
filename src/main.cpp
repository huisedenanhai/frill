#include <filesystem>
#include <fire-hpp/fire.hpp>
#include <fstream>
#include <iostream>
#include <json/json.hpp>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

namespace json = nlohmann;
namespace fs = std::filesystem;

static std::string read_file_str(const fs::path &file_path) {
  std::ifstream t(file_path);
  if (!t.good()) {
    std::stringstream ss;
    ss << "failed to open " << file_path;
    throw std::runtime_error(ss.str());
  }
  std::string str((std::istreambuf_iterator<char>(t)),
                  std::istreambuf_iterator<char>());
  return str;
}

struct Target {
  fs::path relative_path; // relative to input directory
  fs::path absolute_path;
  std::vector<fs::path> include_dirs;

  std::string display() {
    std::stringstream ss;
    ss << "{" << std::endl;
    ss << "\trelative_path:\t" << relative_path << "," << std::endl;
    ss << "\tabsolute_path:\t" << absolute_path << "," << std::endl;
    ss << "\tinclude_dirs: [" << std::endl;
    for (auto &inc : include_dirs) {
      ss << "\t\t" << inc << "," << std::endl;
    }
    ss << "\t]" << std::endl;
    ss << "}";
    return ss.str();
  }
};

struct Config {
  std::vector<Target> targets;
};

static json::json parse_json(const std::string &str) {
  return json::json::parse(str, nullptr, true, true);
}

static Config load_directory(const fs::path &dir_path,
                             const fs::path &project_root) {
  json::json frill;
  auto get_absolute = [&](const fs::path &p) {
    return fs::canonical(fs::absolute(dir_path / p));
  };

  auto frill_file_path = get_absolute("frill.json");
  try {
    auto frill_str = read_file_str(frill_file_path);
    frill = parse_json(frill_str);
  } catch (const std::exception &e) {
    std::stringstream ss;
    ss << "failed to load config " << frill_file_path << ": " << e.what();
    throw std::runtime_error(ss.str());
  }

  Config config{};
  for (auto &target_config : frill["sources"]) {
    if (target_config.is_string()) {
      Target t;
      t.absolute_path = get_absolute(target_config.get<std::string>());
      t.relative_path = fs::relative(t.absolute_path, project_root);
      config.targets.emplace_back(std::move(t));
    } else {
    }
  }

  for (auto &t : config.targets) {
    std::cout << t.display() << std::endl;
  }

  return config;
}

static int fired_main(std::string src_dir = fire::arg(
                          {"-S", "--source-dir", "source file directory"}, "."),
                      std::string dst_dir = fire::arg(
                          {"-B", "--output-directory", "output directory"},
                          ".")) {
  std::cout << src_dir << std::endl;
  std::cout << dst_dir << std::endl;

  try {
    load_directory(src_dir, src_dir);
  } catch (const std::exception &e) {
    std::cout << e.what() << std::endl;
    return -1;
  }
  return 0;
}

FIRE(fired_main, "GLSL shader build system")
