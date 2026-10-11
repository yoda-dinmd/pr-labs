cmake_minimum_required(VERSION 3.20)
project(ConcurrencyLab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
add_executable(working src/main.cpp)
if(UNIX)
    add_executable(benchmark src/benchmark.cpp)
endif()
target_link_libraries(working PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(working PRIVATE psapi)
endif()
if(MSVC)
    target_compile_options(working PRIVATE /W4 /WX)
else()
    foreach(target working benchmark)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wpedantic -Werror -Wshadow
                -Wconversion -Wsign-conversion -Wnull-dereference)
        endif()
    endforeach()
endif()

# Optional full-size correctness check; not a USB benchmark.
include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        if(TARGET benchmark)
            add_test(NAME lab_regression
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/regression.py
                    $<TARGET_FILE:working> $<TARGET_FILE:benchmark>)
        else()
            add_test(NAME lab_regression
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/regression.py $<TARGET_FILE:working>)
        endif()
        set_tests_properties(lab_regression PROPERTIES TIMEOUT 600)
    endif()
endif()
