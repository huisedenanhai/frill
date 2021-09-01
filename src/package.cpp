#include "package.hpp"
#include <fstream>

void package_to_hpp(const frill::fs::path &shader_folder,
                    const frill::fs::path &output_file) {
  std::ofstream os(output_file);
  os << "// this file is generated by frill, do not edit it manually"
     << std::endl;
  os << "#ifndef FRILL_SHADERS_H" << std::endl;
  os << "#define FRILL_SHADERS_H" << std::endl;
  os << std::endl;
  os << std::endl;
  os << "#endif // FRILL_SHADERS_H" << std::endl;
  os.close();
}