cmake_minimum_required(VERSION 3.20)
project(pak_resolver_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(pak_resolver_test
    test.cpp
    ../../src/pak_resolver.cpp
)
target_include_directories(pak_resolver_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME pak_resolver COMMAND pak_resolver_test)
