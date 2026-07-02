add_library(sockets ../src/socket.cpp)
add_library(parsers ../src/http/HttpRequestParser.cpp)
target_include_directories(sockets PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(parsers PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_executable(tests tests.cpp HttpRequestParserTest.cpp)
target_compile_features(tests PRIVATE cxx_std_17)

add_subdirectory(integration)

target_link_libraries(tests
    PRIVATE
        GTest::gtest_main
        sockets
        parsers
)

# CTest 등록
include(GoogleTest)
gtest_discover_tests(tests)
