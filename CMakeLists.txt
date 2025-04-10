cmake_minimum_required(VERSION 3.15)

project(
    Kvbase
    VERSION 1.0
    LANGUAGES CXX)
    
find_package(spdlog REQUIRED)

add_executable(KvServer src/main.cpp)

target_link_libraries(KvServer PRIVATE spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>)
