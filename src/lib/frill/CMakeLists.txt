add_library(frill_archive
        archive.cpp
        archive.hpp)
target_compile_features(frill_archive PRIVATE cxx_std_17)
target_link_libraries(frill_archive PRIVATE json frill_misc)
target_include_directories(frill_archive INTERFACE ..)

add_subdirectory(misc)