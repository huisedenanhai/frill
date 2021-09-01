#pragma once

#include "frill/misc/fs.hpp"
#include "target.hpp"
#include <vector>

void package_to_hpp(const frill::fs::path &shader_folder,
                    const frill::fs::path &output_file);