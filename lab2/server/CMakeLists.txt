cmake_minimum_required(VERSION 3.20)

project(MyProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

function(add_quiz target path)

    add_executable(${target} ${path})

    target_include_directories(${target} PRIVATE include)

    target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json)

    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
    )

endfunction()

add_quiz(server src/server.cpp)
add_quiz(jsontest src/jsontest.cpp)
