add_executable(tests tests.cpp)
target_compile_features(tests PRIVATE cxx_std_17)

add_test(NAME dummy COMMAND tests)