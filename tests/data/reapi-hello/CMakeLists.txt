cmake_minimum_required(VERSION 3.16)
project(reapi_hello CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(reapi_hello
    main.cpp
    math.cpp
    string_util.cpp
    printer.cpp
)
