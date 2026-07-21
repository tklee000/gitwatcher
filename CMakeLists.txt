cmake_minimum_required(VERSION 3.16)

project(GitWatcher VERSION 0.1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GITWATCHER_DISPLAY_VERSION
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
)
configure_file(
    resources/Version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h"
    @ONLY
)

add_executable(GitWatcher WIN32
    src/main.cpp
    src/DiffEngine.cpp
    src/GitRepository.cpp
    src/FileWatcher.cpp
    resources/GitWatcher.rc
)

target_include_directories(GitWatcher PRIVATE
    src
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_definitions(GitWatcher PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(GitWatcher PRIVATE /W4 /permissive- /utf-8)
    target_link_options(GitWatcher PRIVATE /MANIFEST:NO)
endif()

target_link_libraries(GitWatcher PRIVATE comctl32 shell32 ole32)

include(CTest)
if(BUILD_TESTING)
    add_executable(DiffEngineTests
        tests/DiffEngineTests.cpp
        src/DiffEngine.cpp
    )
    target_include_directories(DiffEngineTests PRIVATE src)
    target_compile_definitions(DiffEngineTests PRIVATE NOMINMAX)
    if(MSVC)
        target_compile_options(DiffEngineTests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME DiffEngineTests COMMAND DiffEngineTests)
endif()
