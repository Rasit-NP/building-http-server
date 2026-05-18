add_executable(tests tests.cpp)
target_compile_features(tests PRIVATE cxx_std_17)

target_link_libraries(tests
    PRIVATE
        GTest::gtest_main
        calculator
)

# CTest 등록
include(GoogleTest)
gtest_discover_tests(tests)