#include "target.hpp"
#include <iostream>
#include <json/json.hpp>
#include <sstream>

#include <shaderc/shaderc.hpp>

namespace json = nlohmann;

void raise_error(const fs::path &config, const std::string &err) {
  std::stringstream ss;
  ss << "error: " << config << ": " << err;
  throw std::runtime_error(ss.str());
}

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
        auto content = frill::read_file_str(absolute_include);
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

std::string Target::display() const {
  std::stringstream ss;
  ss << "{" << std::endl;
  ss << "\trelative_path:\t" << relative_path << "," << std::endl;
  ss << "\tabsolute_path:\t" << id.path << "," << std::endl;
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
fs::path Target::output_file_relative_dir(const std::string &ext) const {
  return uid + ext;
}

bool Target::check_outdated(const fs::path &cache_path) const {
  auto ts_path = get_time_stamp_path(cache_path);
  if (!fs::exists(ts_path)) {
    return true;
  }
  try {
    auto js = json::json::parse(frill::read_file_str(ts_path));
    frill::TargetId cache_id{};
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

void Target::compile(const fs::path &output_dir,
                     const fs::path &cache_path) const {
  shaderc::Compiler compiler{};
  shaderc::CompileOptions options{};
  for (auto &flag : id.flags) {
    options.AddMacroDefinition(flag);
  }

  std::map<fs::path, std::string> dep_time_stamps;
  dep_time_stamps.emplace(id.path, last_write_time_str(id.path));

  options.SetIncluder(
      std::make_unique<IncludeCallback>(include_dirs, &dep_time_stamps));

  auto src = frill::read_file_str(id.path);
  auto ext = id.path.extension().string();
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

  auto result =
      compiler.CompileGlslToSpv(src, kind, id.path.string().c_str(), options);

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
    frill::write_file(fs::absolute(output_dir), beg, end - beg);

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
    frill::write_file(get_time_stamp_path(cache_path), js.dump(2));
  }
}

fs::path Target::get_time_stamp_path(const fs::path &cache_path) const {
  return cache_path / output_file_relative_dir(".tm.json");
}
