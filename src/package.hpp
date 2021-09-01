#pragma once

#include "frill/misc/fs.hpp"
#include <vector>

void package_to_hpp(const frill::fs::path &shader_folder,
                    const frill::fs::path &cache_folder,
                    const frill::fs::path &output_file);

bool header_file_outdated(const frill::fs::path &cache_folder,
                          const frill::fs::path &output_file);