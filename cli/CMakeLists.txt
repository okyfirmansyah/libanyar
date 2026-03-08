cmake_minimum_required(VERSION 3.16)
project(anyar_cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(anyar
    src/main.cpp
    src/cmd_init.cpp
    src/cmd_dev.cpp
    src/cmd_build.cpp
    src/util.cpp
    src/templates.cpp
)

target_include_directories(anyar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# We need nlohmann/json for config parsing
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
    PATHS /usr/local/include /usr/include
)
if(NLOHMANN_JSON_INCLUDE_DIR)
    target_include_directories(anyar PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()

find_package(Threads REQUIRED)
target_link_libraries(anyar PRIVATE Threads::Threads)

# Filesystem library (GCC <9 needs explicit stdc++fs)
target_link_libraries(anyar PRIVATE stdc++fs)

# Install the CLI binary
install(TARGETS anyar RUNTIME DESTINATION bin)
