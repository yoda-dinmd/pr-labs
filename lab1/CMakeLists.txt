cmake_minimum_required(VERSION 3.20)

project(MyProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(add_quiz target path)

    add_executable(${target} ${path})

    target_include_directories(${target} PRIVATE include)

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX
        )
    else()
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
    endif()

endfunction()

add_quiz(working src/main.cpp)
