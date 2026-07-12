add_executable(http_server
        main.cpp socket.cpp connection.cpp event_loop.cpp
        http/HttpRequestParser.cpp
)

target_include_directories(http_server
    PRIVATE ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(http_server PRIVATE cxx_std_17)

target_compile_options(http_server PRIVATE
    -Wall -Wextra -Wpedantic
)