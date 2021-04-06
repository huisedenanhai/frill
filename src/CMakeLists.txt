add_executable(frill main.cpp)
target_compile_features(frill PRIVATE cxx_std_17)
target_link_libraries(frill PRIVATE
        fire-hpp
        json
        shaderc
        frill_misc)

target_include_directories(frill PRIVATE lib)

add_subdirectory(lib/frill)