add_executable(echo_client echo_client.cpp)
target_compile_features(echo_client PRIVATE cxx_std_17)

target_compile_definitions(echo_client PRIVATE
    SERVER_BINARY_PATH="$<TARGET_FILE:http_server>"
)

add_test(NAME integration.single_echo
         COMMAND echo_client single_echo)
add_test(NAME integration.concurrent_echo
         COMMAND echo_client concurrent_echo)
add_test(NAME integration.large_payload
         COMMAND echo_client large_payload)
add_test(NAME integration.idle_cpu
         COMMAND echo_client idle_cpu)
add_test(NAME integration.signal_shutdown
         COMMAND echo_client signal_shutdown)


set_tests_properties(
        integration.signal_shutdown
        integration.idle_cpu
        PROPERTIES TIMEOUT 30
)