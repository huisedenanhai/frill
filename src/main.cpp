#include <chrono>
#include <fire-hpp/fire.hpp>
#include <frill/misc/fs.hpp>
#include <frill/misc/target_id.hpp>
#include <frill/misc/thread_pool.hpp>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "package.hpp"
#include "target.hpp"

namespace json = nlohmann;
namespace fs = frill::fs;

namespace std {
template <> struct hash<Target> {
  size_t operator()(const Target &target) const {
    return hash<frill::TargetId>()(target.id);
  }
};
} // namespace std

static json::json parse_json(const std::string &str) {
  return json::json::parse(str, nullptr, true, true);
}

static std::unordered_set<Target>
load_directory(const fs::path &dir_path,
               const fs::path &project_root,
               const std::set<fs::path> &parent_includes) {
  json::json frill;
  auto get_absolute = [&](const fs::path &p) {
    return fs::canonical(fs::absolute(dir_path / p));
  };

  auto frill_file_path = get_absolute("frill.json");
  try {
    auto frill_str = frill::read_file_str(frill_file_path);
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
          include_dirs.insert(get_absolute(inc.get<std::string>()));
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
            t.id.path = absolute_path;
            t.relative_path = relative_path;
            t.include_dirs = include_dirs;
            t.declaring_config_file = frill_file_path;
            for (auto flag : flag_combination) {
              if (strcmp(flag, "") != 0) {
                t.id.flags.insert(flag);
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

  auto current_includes = parent_includes;

  for (auto &inc : frill["includes"]) {
    if (inc.is_string()) {
      current_includes.insert(get_absolute(inc.get<std::string>()));
    } else {
      raise_error(frill_file_path,
                  "include directories should be specified with string");
    }
  }

  for (auto &t : targets) {
    t.include_dirs.insert(current_includes.begin(), current_includes.end());
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
        auto targets_in_subdir =
            load_directory(get_absolute(subdir.get<std::string>()),
                           project_root,
                           current_includes);
        add_unique_targets(targets_in_subdir);
      }
    }
  }

  return unique_targets;
}

void compile_glsl_to_spv(ThreadPool *thread_pool,
                         const std::vector<Target> &outdated_targets,
                         const fs::path &dst_path,
                         const fs::path &cache_path) {
  std::vector<std::future<void>> futures;
  futures.reserve(outdated_targets.size());

  {
    size_t index = 0;
    for (auto &target : outdated_targets) {
      futures.emplace_back(
          thread_pool->schedule([current_index = index,
                                 task_count = outdated_targets.size(),
                                 &target,
                                 &dst_path,
                                 &cache_path]() {
            try {
              {
                std::stringstream ss;
                ss << target.id.path;
                for (auto &flag : target.id.flags) {
                  ss << " " << flag;
                }
                printf("[%zu/%zu] compiling %s\n",
                       current_index + 1,
                       task_count,
                       ss.str().c_str());
              }
              target.compile(dst_path / target.output_file_relative_dir(),
                             cache_path);
            } catch (std::exception &e) {
              fprintf(stderr, "%s\n", e.what());
            }
          }));

      index++;
    }
  }

  for (auto &fut : futures) {
    fut.wait();
  }
}

static void assign_uid(std::vector<Target> &targets) {
  std::set<size_t> id;
  std::function<size_t(const std::string &)> get_unique_uid =
      [&](const std::string &name) {
        auto h = std::hash<std::string>()(name);
        if (id.count(h) != 0) {
          return get_unique_uid(name + "+");
        }
        id.insert(h);
        return h;
      };
  for (auto &t : targets) {
    std::stringstream ss;
    ss << t.id.path;
    for (auto &f : t.id.flags) {
      ss << "." << f;
    }
    t.uid = std::to_string(get_unique_uid(ss.str()));
  }
}

static frill::IndexFile build_index(const std::vector<Target> &targets) {
  frill::IndexFile index{};
  for (auto &t : targets) {
    frill::IndexTerm term{};
    frill::TargetId &id = term.target;
    id.path = t.relative_path;
    id.flags = t.id.flags;
    term.uid = t.uid;
    index.targets.push_back(std::move(term));
  }
  return index;
}

static int
fired_main(const std::string &src_dir =
               fire::arg({"-S", "--source-dir", "source file directory"}, "."),
           const std::string &dst_dir =
               fire::arg({"-B", "--output-dir", "output directory"}, "."),
           const std::string &cache_dir =
               fire::arg({"-C", "--cache-dir", "cache directory"}, "<dst_dir>"),
           unsigned int thread_count =
               fire::arg({"-j", "--thread-count", "worker thread count"},
                         std::thread::hardware_concurrency())) {
  // command line arguments are already in the target encoding
  fs::path src_path = std::filesystem::path(src_dir);
  fs::path dst_path = std::filesystem::path(dst_dir);
  fs::path cache_path;
  if (cache_dir == "<dst_dir>") {
    cache_path = dst_path;
  } else {
    cache_path = std::filesystem::path(cache_dir);
  }
  cache_path /= "__frill_cache__";
  try {
    std::vector<Target> targets;
    {
      auto unique_targets = load_directory(src_path, src_path, {});
      targets.insert(
          targets.end(), unique_targets.begin(), unique_targets.end());
    }
    assign_uid(targets);

    std::vector<Target> outdated_targets{};
    for (auto &t : targets) {
      if (t.check_outdated(cache_path)) {
        outdated_targets.push_back(t);
      }
    }

    // update index no matter build outdated or not
    auto index = build_index(targets);
    frill::save_json_file(index, dst_path / "index.json");

    if (outdated_targets.empty()) {
      std::cout << "all targets updated, nothing to compile" << std::endl;
    } else {
      auto thread_pool = std::make_unique<ThreadPool>(thread_count);
      compile_glsl_to_spv(
          thread_pool.get(), outdated_targets, dst_path, cache_path);
    }

    // TODO do package on demand
    auto header_path = dst_path / "frill_shaders.hpp";
    package_to_hpp(dst_path, header_path);
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }
  return 0;
}

FIRE(fired_main, "GLSL shader build system")
