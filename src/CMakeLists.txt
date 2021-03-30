add_executable(frill main.cpp)

target_compile_features(frill PRIVATE cxx_std_17)

target_link_libraries(frill PRIVATE fire-hpp json)
