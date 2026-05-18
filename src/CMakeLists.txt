add_library(calculator STATIC calculator.cpp)
target_include_directories(calculator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(http_server main.cpp)
target_link_libraries(http_server PRIVATE calculator)

target_include_directories(http_server
    PRIVATE ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(http_server PRIVATE cxx_std_17)

target_compile_options(http_server PRIVATE
    -Wall -Wextra -Wpedantic
)