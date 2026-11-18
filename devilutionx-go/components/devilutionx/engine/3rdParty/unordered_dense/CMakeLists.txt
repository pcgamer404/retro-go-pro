add_library(unordered_dense INTERFACE)
target_include_directories(unordered_dense INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(ankerl::unordered_dense ALIAS unordered_dense)
add_library(unordered_dense::unordered_dense ALIAS unordered_dense)
