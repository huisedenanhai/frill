add_executable(frill main.cpp thread_pool.hpp)
target_compile_features(frill PRIVATE cxx_std_17)
target_link_libraries(frill PRIVATE fire-hpp json shaderc)

add_library(frill_archive archive.cpp archive.hpp)
target_compile_features(frill_archive PRIVATE cxx_std_17)
target_link_libraries(frill_archive PRIVATE json)
