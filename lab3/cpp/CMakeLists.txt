cmake_minimum_required(VERSION 3.20)

project(LanChat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenSSL REQUIRED)

find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

set(CHAT_COMPILE_OPTIONS
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
)

add_library(chat_core STATIC
    src/Logger.cpp
    src/Crypto.cpp
    src/Protocol.cpp
    src/UdpSocket.cpp
    src/PeerRegistry.cpp
    src/ConsoleUI.cpp
    src/ChatPeer.cpp
)

target_include_directories(chat_core PUBLIC include)
target_link_libraries(chat_core PUBLIC OpenSSL::Crypto nlohmann_json::nlohmann_json)
target_compile_options(chat_core PRIVATE ${CHAT_COMPILE_OPTIONS})

add_executable(chat src/main.cpp)
target_link_libraries(chat PRIVATE chat_core)
target_compile_options(chat PRIVATE ${CHAT_COMPILE_OPTIONS})

enable_testing()

add_executable(chat_tests tests/test_main.cpp)
target_link_libraries(chat_tests PRIVATE chat_core)
target_compile_options(chat_tests PRIVATE ${CHAT_COMPILE_OPTIONS})

add_test(NAME chat_tests COMMAND chat_tests)
