#include "thread_pool.hpp"
#include <chrono>
#include <filesystem>
#include <fire-hpp/fire.hpp>
#include <fstream>
#include <functional>
#include <iostream>
#include <json/json.hpp>
#include <optional>
#include <set>
#include <shaderc/shaderc.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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

static void write_file(const fs::path &file_path, const std::string &str) {
  write_file(file_path, str.c_str(), str.size());
}

struct TargetId {
  fs::path absolute_path;
  std::set<std::string> flags;

  json::json to_json() const {
    json::json js{};
    js["path"] = absolute_path;
    auto flags_js = json::json::array();
    for (auto &flag : flags) {
      flags_js.push_back(flag);
    }
    js["flags"] = flags_js;
    return js;
  }

  void load_json(const json::json &js) {
    absolute_path = js["path"].get<std::string>();
    flags.clear();
    for (auto &flag : js["flags"]) {
      flags.insert(flag.get<std::string>());
    }
  }

  bool operator==(const TargetId &rhs) const {
    return absolute_path == rhs.absolute_path && flags == rhs.flags;
  }

  bool operator!=(const TargetId &rhs) const {
    return !(*this == rhs);
  }
};

template <typename TP> std::time_t to_time_t(TP tp) {
  using namespace std::chrono;
  auto sctp = time_point_cast<system_clock::duration>(tp - TP::clock::now() +
                                                      system_clock::now());
  return system_clock::to_time_t(sctp);
}

static std::string time_to_string(const fs::file_time_type &time_point) {
  std::stringstream ss;
  auto tm = to_time_t(time_point);
  ss << std::asctime(std::localtime(&tm));
  return ss.str();
}

static std::string last_write_time_str(const fs::path &path) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> guard(mutex);
  // fs::last_write_time is not thread safe
  return time_to_string(fs::last_write_time(path));
}

class IncludeCallback : public shaderc::CompileOptions::IncluderInterface {
public:
  IncludeCallback(std::set<fs::path> include_dirs,
                  std::map<fs::path, std::string> *dep_time_stamps)
      : _include_dirs(std::move(include_dirs)),
        _dep_time_stamps(dep_time_stamps) {}

  // Handles shaderc_include_resolver_fn callbacks.
  shaderc_include_result *GetInclude(const char *requested_source,
                                     shaderc_include_type type,
                                     const char *requesting_source,
                                     size_t include_depth) override {
    constexpr size_t max_include_depth = 50;
    if (include_depth > max_include_depth) {
      std::stringstream ss;
      ss << "include depth exceeds " << max_include_depth;
      return include_error(ss.str());
    }
    auto absolute_include =
        resolve_include(requested_source, type, requesting_source);
    if (absolute_include.has_value()) {
      _dep_time_stamps->emplace(*absolute_include,
                                last_write_time_str(*absolute_include));
      return load_include(*absolute_include);
    }
    return include_error("failed to resolve include");
  }

  // Handles shaderc_include_result_release_fn callbacks.
  void ReleaseInclude(shaderc_include_result *data) override {}

private:
  shaderc_include_result *load_include(const fs::path &absolute_include) {
    auto inc_path = absolute_include.string();
    auto it = _results.find(inc_path);
    if (it == _results.end()) {
      try {
        auto content = read_file_str(absolute_include);
        _results[inc_path] = std::make_unique<IncludeResult>(inc_path, content);
      } catch (std::exception &e) {
        return include_error(e.what());
      }
    }
    return &_results[inc_path]->result;
  }

  shaderc_include_result *include_error(const std::string &err) {
    auto error = std::make_unique<IncludeResult>(err);
    auto res = &error->result;
    _errors.emplace_back(std::move(error));
    return res;
  }

  std::optional<fs::path> resolve_include(const fs::path &requested_source,
                                          shaderc_include_type type,
                                          const fs::path &requesting_source) {
    switch (type) {
    case shaderc_include_type_relative:
      return resolve_relative_include(requested_source, requesting_source);
    case shaderc_include_type_standard:
      return resolve_absolute_include(requested_source);
    }
    return std::nullopt;
  }

  std::optional<fs::path>
  resolve_relative_include(const fs::path &requested_source,
                           const fs::path &requesting_source) {
    auto dir = requesting_source.parent_path() / requested_source;
    if (fs::exists(dir)) {
      return fs::canonical(dir);
    }
    return resolve_absolute_include(requested_source);
  }

  std::optional<fs::path>
  resolve_absolute_include(const fs::path &requested_source) {
    for (auto &inc : _include_dirs) {
      auto dir = inc / requested_source;
      if (fs::exists(dir)) {
        return fs::canonical(dir);
      }
    }
    return std::nullopt;
  }

  struct IncludeResult {
    shaderc_include_result result{};
    struct Data {
      std::string name;
      std::string content;
    };
    std::unique_ptr<Data> data{};

    IncludeResult(std::string name, std::string content) {
      data = std::make_unique<Data>();
      data->name = std::move(name);
      data->content = std::move(content);
      result.user_data = nullptr;
      result.source_name = data->name.c_str();
      result.source_name_length = data->name.size();
      result.content = data->content.c_str();
      result.content_length = data->content.size();
    }

    explicit IncludeResult(std::string err) {
      data = std::make_unique<Data>();
      data->content = std::move(err);
      result.user_data = nullptr;
      result.source_name = nullptr;
      result.source_name_length = 0;
      result.content = data->content.c_str();
      result.content_length = data->content.size();
    }
  };

  std::unordered_map<std::string, std::unique_ptr<IncludeResult>> _results;
  std::set<fs::path> _include_dirs;
  std::map<fs::path, std::string> *_dep_time_stamps;
  std::vector<std::unique_ptr<IncludeResult>> _errors;
};

struct Target {
  fs::path relative_path; // relative to input directory
  TargetId id{};
  std::set<fs::path> include_dirs;
  fs::path declaring_config_file; // absolute path

  std::string display() const {
    std::stringstream ss;
    ss << "{" << std::endl;
    ss << "\trelative_path:\t" << relative_path << "," << std::endl;
    ss << "\tabsolute_path:\t" << id.absolute_path << "," << std::endl;
    ss << "\tinclude_dirs: [" << std::endl;
    for (auto &inc : include_dirs) {
      ss << "\t\t" << inc << "," << std::endl;
    }
    ss << "\t]," << std::endl;
    ss << "\tflags: [" << std::endl;
    for (auto &flag : id.flags) {
      ss << "\t\t" << flag << "," << std::endl;
    }
    ss << "\t]," << std::endl;
    ss << "\tdeclaring_config_file: " << declaring_config_file << std::endl;
    ss << "}";
    return ss.str();
  }

  fs::path output_file_relative_dir(const std::string &ext = ".spv") const {
    std::stringstream ss;
    ss << relative_path.c_str();
    for (auto &flag : id.flags) {
      ss << "." << flag;
    }
    ss << ext;
    return ss.str();
  }

  bool operator==(const Target &rhs) const {
    return id == rhs.id;
  }

  bool check_outdated(const fs::path &cache_path) const {
    auto ts_path = get_time_stamp_path(cache_path);
    if (!fs::exists(ts_path)) {
      return true;
    }
    try {
      auto js = json::json::parse(read_file_str(ts_path));
      TargetId cache_id{};
      cache_id.load_json(js["target"]);
      if (cache_id != id) {
        return true;
      }
      for (auto &dep : js["deps"]) {
        auto p = dep["path"].get<std::string>();
        auto ts = dep["time_stamp"].get<std::string>();
        if (!fs::exists(p) || last_write_time_str(p) != ts) {
          return true;
        }
      }
      auto recorded_incs = js["includes"];
      if (recorded_incs.size() != include_dirs.size()) {
        return true;
      }
      for (const auto &inc : recorded_incs) {
        if (include_dirs.count(inc.get<std::string>()) == 0) {
          return true;
        }
      }

      return false;
    } catch (std::exception &e) {
      std::cout << "[DEV LOG] failed to load cache " << ts_path << ": "
                << e.what() << std::endl;
    }
    return true;
  }

  // return dependencies
  void compile(const fs::path &output_dir, const fs::path &cache_path) const {
    shaderc::Compiler compiler{};
    shaderc::CompileOptions options{};
    for (auto &flag : id.flags) {
      options.AddMacroDefinition(flag);
    }

    std::map<fs::path, std::string> dep_time_stamps;
    dep_time_stamps.emplace(id.absolute_path,
                            last_write_time_str(id.absolute_path));

    options.SetIncluder(
        std::make_unique<IncludeCallback>(include_dirs, &dep_time_stamps));

    auto src = read_file_str(id.absolute_path);
    auto ext = id.absolute_path.extension().string();
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

    auto result = compiler.CompileGlslToSpv(
        src, kind, (const char *)id.absolute_path.c_str(), options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
      std::stringstream ss;
      ss << "flags: [";
      {
        size_t i = 0;
        for (auto &flag : id.flags) {
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
      write_file(fs::absolute(output_dir), beg, end - beg);

      json::json js;
      js["target"] = id.to_json();
      {
        auto deps = json::json::array();
        // also add output file to deps
        auto output_canonical = fs::canonical(fs::absolute(output_dir));
        dep_time_stamps.emplace(output_canonical,
                                last_write_time_str(output_canonical));
        for (auto &dep : dep_time_stamps) {
          json::json term;
          term["path"] = dep.first.string();
          term["time_stamp"] = dep.second;
          deps.push_back(term);
        }
        js["deps"] = deps;
      }
      {
        auto incs = json::json::array();
        for (auto &inc : include_dirs) {
          incs.push_back(inc.string());
        }
        js["includes"] = incs;
      }
      write_file(get_time_stamp_path(cache_path), js.dump(2));
    }
  }

private:
  fs::path get_time_stamp_path(const fs::path &cache_path) const {
    return cache_path / output_file_relative_dir(".tm.json");
  }
};

template <typename T>
inline void hash_combine(std::size_t &seed, const T &val) {
  std::hash<T> hasher;
  seed ^= hasher(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

namespace std {
template <> struct hash<TargetId> {
  size_t operator()(const TargetId &id) const {
    size_t seed = 0;
    hash_combine(seed, id.absolute_path.string());
    for (auto &flag : id.flags) {
      hash_combine(seed, flag);
    }
    return seed;
  }
};

template <> struct hash<Target> {
  size_t operator()(const Target &target) const {
    return hash<TargetId>()(target.id);
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
            t.id.absolute_path = absolute_path;
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
                ss << target.id.absolute_path;
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
  fs::path src_path = src_dir;
  fs::path dst_path = dst_dir;
  fs::path cache_path;
  if (cache_dir == "<dst_dir>") {
    cache_path = dst_path;
  } else {
    cache_path = cache_dir;
  }
  cache_path /= "__frill_cache__";
  try {
    auto targets = load_directory(src_path, src_path, {});

    std::vector<Target> outdated_targets{};
    for (auto &t : targets) {
      if (t.check_outdated(cache_path)) {
        outdated_targets.push_back(t);
      }
    }

    if (outdated_targets.empty()) {
      std::cout << "all targets updated, nothing to compile" << std::endl;
      return 0;
    }

    auto thread_pool = std::make_unique<ThreadPool>(thread_count);
    compile_glsl_to_spv(
        thread_pool.get(), outdated_targets, dst_path, cache_path);
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }
  return 0;
}

FIRE(fired_main, "GLSL shader build system")
