add_library(frill_misc
        target_id.cpp
        target_id.hpp
        thread_pool.hpp
        fs.cpp
        fs.hpp
        )
target_compile_features(frill_misc PRIVATE cxx_std_17)
target_link_libraries(frill_misc PRIVATE json)
