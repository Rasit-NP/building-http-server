add_library(sockets ../src/socket.cpp)
target_include_directories(sockets PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_executable(tests tests.cpp)
target_compile_features(tests PRIVATE cxx_std_17)

target_link_libraries(tests
    PRIVATE
        GTest::gtest_main
        sockets
)

# CTest 등록
include(GoogleTest)
gtest_discover_tests(tests)
