cmake_minimum_required(VERSION 2.8) 
add_compile_options(-std=c++17)


set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/brokers/)

add_executable (storage_broker main.cpp )
target_link_libraries (storage_broker LINK_PUBLIC brokers_common imtjson simpleServer)
