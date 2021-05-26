project(log)

add_library(log log.cc)
target_include_directories(log PUBLIC ${PROJECT_SOURCE_DIR}/include)

add_executable(log_test log_test.cc)
target_link_libraries(log_test log)