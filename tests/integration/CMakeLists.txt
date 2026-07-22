add_executable(echo_client echo_client.cpp)
target_compile_features(echo_client PRIVATE cxx_std_17)

target_compile_definitions(echo_client PRIVATE
    SERVER_BINARY_PATH="$<TARGET_FILE:http_server>"
)

# 서버가 echo → HTTP로 바뀌어 아래 시나리오들은 echo 계약이 성립하지 않는다.
# HTTP 계약으로 재작성할 때까지 등록을 보류한다. (echo_client 자체는 계속 빌드)
# add_test(NAME integration.single_echo
#          COMMAND echo_client single_echo)
# add_test(NAME integration.concurrent_echo
#          COMMAND echo_client concurrent_echo)
# add_test(NAME integration.large_payload
#          COMMAND echo_client large_payload)
# add_test(NAME integration.signal_shutdown
#          COMMAND echo_client signal_shutdown)

add_test(NAME integration.idle_cpu
         COMMAND echo_client idle_cpu)


set_tests_properties(
        integration.idle_cpu
        PROPERTIES TIMEOUT 30
)