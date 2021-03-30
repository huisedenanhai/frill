#include "thread_pool.hpp"
#include <filesystem>
#include <fire-hpp/fire.hpp>
#include <fstream>
#include <functional>
#include <iostream>
#include <json/json.hpp>
#include <set>
#include <shaderc/shaderc.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace json = nlohmann;
namespace fs = std::filesystem;

static void raise_error(const fs::path &config, const std::string &err) {
  std::stringstream ss;
  ss << "error: " << config << ": " << err;
  throw std::runtime_error(ss.str());
}

static std::string read_file_str(const fs::path &file_path) {
  std::ifstream t(file_path);
  if (!t.good()) {
    std::stringstream ss;
    ss << "failed to open " << file_path;
    throw std::runtime_error(ss.str());
  }
  std::string str((std::istreambuf_iterator<char>(t)),
                  std::istreambuf_iterator<char>());
  t.close();
  return str;
}

static void
write_file(const fs::path &file_path, const char *data, size_t size) {
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

struct Target {
  fs::path relative_path; // relative to input directory
  fs::path absolute_path;
  std::set<fs::path> include_dirs;
  std::set<std::string> flags;
  fs::path declaring_config_file; // absolute path

  std::string display() const {
    std::stringstream ss;
    ss << "{" << std::endl;
    ss << "\trelative_path:\t" << relative_path << "," << std::endl;
    ss << "\tabsolute_path:\t" << absolute_path << "," << std::endl;
    ss << "\tinclude_dirs: [" << std::endl;
    for (auto &inc : include_dirs) {
      ss << "\t\t" << inc << "," << std::endl;
    }
    ss << "\t]," << std::endl;
    ss << "\tflags: [" << std::endl;
    for (auto &flag : flags) {
      ss << "\t\t" << flag << "," << std::endl;
    }
    ss << "\t]," << std::endl;
    ss << "\tdeclaring_config_file: " << declaring_config_file << std::endl;
    ss << "}";
    return ss.str();
  }

  fs::path output_file_relative_dir() const {
    std::stringstream ss;
    ss << relative_path.c_str();
    for (auto &flag : flags) {
      ss << "." << flag;
    }
    ss << ".spv";
    return ss.str();
  }

  bool operator==(const Target &rhs) const {
    return absolute_path == rhs.absolute_path && flags == rhs.flags;
  }
};

template <typename T>
inline void hash_combine(std::size_t &seed, const T &val) {
  std::hash<T> hasher;
  seed ^= hasher(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

namespace std {
template <> struct hash<Target> {
  size_t operator()(const Target &target) const {
    size_t seed = 0;
    hash_combine(seed, target.absolute_path.string());
    for (auto &flag : target.flags) {
      hash_combine(seed, flag);
    }
    return seed;
  }
};
} // namespace std

static json::json parse_json(const std::string &str) {
  return json::json::parse(str, nullptr, true, true);
}

static std::unordered_set<Target> load_directory(const fs::path &dir_path,
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

  std::vector<Target> targets{};

  auto add_targets = [&](const json::json &target_config) {
    if (!target_config["file"].is_string()) {
      raise_error(frill_file_path, "targets requires file name");
      return;
    }
    auto absolute_path = get_absolute(target_config["file"].get<std::string>());
    auto relative_path = fs::relative(absolute_path, project_root);
    std::set<fs::path> include_dirs;
    if (target_config.contains("includes")) {
      for (auto &inc : target_config["includes"]) {
        if (inc.is_string()) {
          include_dirs.insert(get_absolute(inc));
        } else {
          raise_error(frill_file_path,
                      "include directories should be specified with string");
        }
      }
    }
    std::vector<std::set<std::string>> multi_compiles;
    if (target_config.contains("multi_compile")) {
      for (auto &flag_config : target_config["multi_compile"]) {
        if (flag_config.is_string()) {
          std::set<std::string> flags{"", flag_config.get<std::string>()};
          multi_compiles.emplace_back(std::move(flags));
        } else if (flag_config.is_array()) {
          std::set<std::string> flags{""};
          for (auto &flag : flag_config) {
            if (!flag.is_string()) {
              raise_error(frill_file_path, "flags should be string");
              continue;
            }
            flags.insert(flag.get<std::string>());
          }
          multi_compiles.emplace_back(std::move(flags));
        } else if (flag_config.is_object()) {
          std::set<std::string> flags;
          for (auto &flag : flag_config["options"]) {
            flags.insert(flag.get<std::string>());
          }
          auto can_off_key = "can_off";
          if (flag_config.contains(can_off_key) &&
              flag_config[can_off_key].is_boolean() &&
              !flag_config[can_off_key].get<bool>()) {
          } else {
            flags.insert("");
          }
          multi_compiles.emplace_back(std::move(flags));
        } else {
          raise_error(frill_file_path,
                      "multi-compile options should be specified as "
                      "string/array or object");
        }
      }
    }

    std::vector<const char *> flag_combination{};
    flag_combination.resize(multi_compiles.size());
    std::function<void(size_t)> add_multi_compile_options =
        [&](size_t flags_index) {
          if (flags_index == flag_combination.size()) {
            Target t;
            t.absolute_path = absolute_path;
            t.relative_path = relative_path;
            t.include_dirs = include_dirs;
            t.declaring_config_file = frill_file_path;
            for (auto flag : flag_combination) {
              if (strcmp(flag, "") != 0) {
                t.flags.insert(flag);
              }
            }
            targets.emplace_back(std::move(t));
            return;
          }
          for (auto &flag : multi_compiles[flags_index]) {
            flag_combination[flags_index] = flag.c_str();
            add_multi_compile_options(flags_index + 1);
          }
        };
    add_multi_compile_options(0);
  };

  for (auto &target_config : frill["sources"]) {
    if (target_config.is_string()) {
      json::json conf;
      conf["file"] = target_config.get<std::string>();
      add_targets(conf);
    } else if (target_config.is_object()) {
      add_targets(target_config);
    } else {
      raise_error(frill_file_path,
                  "targets should be specified with string or object");
    }
  }

  for (auto &inc : frill["includes"]) {
    if (inc.is_string()) {
      for (auto &t : targets) {
        t.include_dirs.insert(get_absolute(inc));
      }
    } else {
      raise_error(frill_file_path,
                  "include directories should be specified with string");
    }
  }

  std::unordered_set<Target> unique_targets;
  auto add_unique_targets = [&](auto &&ts) {
    for (auto &target : ts) {
      if (unique_targets.count(target) != 0) {
        std::stringstream ss;
        ss << "target " << target.display() << " is emitted multiple times."
           << std::endl;
        ss << "first in: " << unique_targets.find(target)->declaring_config_file
           << std::endl;
        ss << "second in: " << target.declaring_config_file << std::endl;
        throw std::runtime_error(ss.str());
      }
      unique_targets.insert(target);
    }
  };

  add_unique_targets(targets);

  if (frill.contains("subdirectories")) {
    for (auto &subdir : frill["subdirectories"]) {
      if (!subdir.is_string()) {
        raise_error(frill_file_path,
                    "subdirectories should be specified as strings");
      } else {
        auto targets_in_subdir = load_directory(
            get_absolute(subdir.get<std::string>()), project_root);
        add_unique_targets(targets_in_subdir);
      }
    }
  }

  return unique_targets;
}

static int
fired_main(const std::string &src_dir =
               fire::arg({"-S", "--source-dir", "source file directory"}, "."),
           const std::string &dst_dir =
               fire::arg({"-B", "--output-directory", "output directory"}, "."),
           unsigned int thread_count =
               fire::arg({"-j", "--thread-count", "worker thread count"},
                         std::thread::hardware_concurrency())) {
  try {
    auto targets = load_directory(src_dir, src_dir);
    auto thread_pool = std::make_unique<ThreadPool>(thread_count);
    std::vector<std::future<void>> futures;
    futures.reserve(targets.size());
    {
      size_t index = 0;
      for (auto &target : targets) {
        thread_pool->schedule([index, &target, &dst_dir, &targets]() {
          shaderc::Compiler compiler{};
          shaderc::CompileOptions options{};
          for (auto &flag : target.flags) {
            options.AddMacroDefinition(flag);
          }
          try {
            auto src = read_file_str(target.absolute_path);
            auto ext = target.absolute_path.extension();
            shaderc_shader_kind kind = shaderc_glsl_default_vertex_shader;
            static const std::map<std::string, shaderc_shader_kind> stages = {
                {".vert", shaderc_glsl_default_vertex_shader},
                {".frag", shaderc_glsl_default_fragment_shader},
                {".tesc", shaderc_glsl_default_tess_control_shader},
                {".tese", shaderc_glsl_default_tess_evaluation_shader},
                {".geom", shaderc_glsl_default_geometry_shader},
                {".comp", shaderc_glsl_default_compute_shader},
                {".spvasm", shaderc_spirv_assembly},
                {".rgen", shaderc_glsl_default_raygen_shader},
                {".rahit", shaderc_glsl_default_anyhit_shader},
                {".rchit", shaderc_glsl_default_closesthit_shader},
                {".rmiss", shaderc_glsl_default_miss_shader},
                {".rint", shaderc_glsl_default_intersection_shader},
                {".rcall", shaderc_glsl_default_callable_shader},
                {".task", shaderc_glsl_default_task_shader},
                {".mesh", shaderc_glsl_default_mesh_shader},
            };

            {
              auto it = stages.find(ext);
              if (it != stages.end()) {
                kind = it->second;
              }
            }

            {
              std::stringstream ss;
              ss << target.absolute_path;
              for (auto &flag : target.flags) {
                ss << " " << flag;
              }
              printf("[%zu/%zu] compiling %s\n",
                     index,
                     targets.size(),
                     ss.str().c_str());
            }
            auto result = compiler.CompileGlslToSpv(
                src, kind, target.absolute_path.c_str(), options);
            if (result.GetCompilationStatus() !=
                shaderc_compilation_status_success) {
              std::stringstream ss;
              ss << "flags: [";
              {
                size_t i = 0;
                for (auto &flag : target.flags) {
                  if (i != 0) {
                    ss << ", ";
                  }
                  ss << flag;
                  i++;
                }
              }
              ss << "] ";
              ss << result.GetErrorMessage();
              fprintf(stderr, "%s", ss.str().c_str());
            } else {
              auto beg = reinterpret_cast<const char *>(result.begin());
              auto end = reinterpret_cast<const char *>(result.end());
              write_file(
                  fs::absolute(dst_dir / target.output_file_relative_dir()),
                  beg,
                  end - beg);
            }
          } catch (std::exception &e) {
            // use old style printf for sync with other threads
            fprintf(stderr, "%s\n", e.what());
          }
        });

        index++;
      }
    }
    for (auto &fut : futures) {
      fut.wait();
    }
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }
  return 0;
}

FIRE(fired_main, "GLSL shader build system")
