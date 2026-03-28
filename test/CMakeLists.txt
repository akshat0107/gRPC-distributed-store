cmake_minimum_required(VERSION 3.16)
project(project3 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(run_vendors vendor.cc run_vendors.cc)
target_link_libraries(run_vendors 
	Threads::Threads
	p3protolib
	gRPC::grpc++)
add_dependencies(run_vendors p3protolib)

add_executable(run_tests client.cc run_tests.cc product_queries_util.h)
target_link_libraries(run_tests 
	Threads::Threads
	p3protolib
	gRPC::grpc++
	gRPC::grpc++_reflection)
add_dependencies(run_tests p3protolib)

set_target_properties(run_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/)
set_target_properties(run_vendors PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/)

#product_query_list.txt file symlink rule
set (source "${CMAKE_CURRENT_SOURCE_DIR}/product_query_list.txt")
set (destination "${CMAKE_BINARY_DIR}/bin/product_query_list.txt")
add_custom_command(
        TARGET run_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${source} ${destination}
        DEPENDS ${destination}
        COMMENT "symbolic link ${destination} created."
)
