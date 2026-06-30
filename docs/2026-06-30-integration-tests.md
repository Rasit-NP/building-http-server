# integration tests — 작업 & 트러블슈팅 로그

- **날짜**: 2026-06-30
- **작업**: 서버를 실제 프로세스로 띄워 검증하는 통합 테스트 하니스(`echo_client`) 도입 — 5개 시나리오를 `ctest`에 등록. 하니스 계약을 맞추기 위해 서버에 포트 인자 + `PORT <n>` 출력 + `getsockname` 기반 `local_port()` 추가
- **결과**: Ubuntu 22.04 컨테이너 빌드 성공, `ctest` 11/11 통과(단위 6 + 통합 5), 총 2.03s

---

## 1. 작업 개요

| 항목 | Before | After |
|------|--------|-------|
| 테스트 범위 | `SocketTest` 단위 6종(in-process) | 단위 6 + **블랙박스 통합 5종**(fork+execv로 서버 기동) |
| 서버 포트 | `main`에서 `8080` 하드코딩 | `argv[1]` 파싱(`std::stoul`), 미지정 시 `8080`. 포트 `0` 지원(OS 임시 할당) |
| 포트 통지 | 없음 | `listen()` 직후 stdout에 `PORT <n>\n` 출력 + `fflush(stdout)` |
| 포트 조회 | 없음 | `Socket::local_port()` — `getsockname(2)`로 실제 바인딩 포트 되읽기 |
| ctest 등록 | `gtest_discover_tests`만 | + `tests/integration` 서브디렉터리(`add_test` 5건) |

**통합 시나리오(`tests/integration/echo_client.cpp`)**
1. `single_echo` — 단일 연결 왕복
2. `concurrent_echo` — 8 스레드 동시, 연결별 고유 payload로 연결 간 데이터 섞임 검출
3. `large_payload` — 4MB 전송으로 서버 `EPOLLOUT` backpressure 경로 강제
4. `idle_cpu` — 연결 0 상태로 2초 idle, `/proc/<pid>/stat`의 utime+stime 델타로 busy-spin 회귀 탐지(`/proc` 부재 시 77 반환)
5. `signal_shutdown` — active 연결 유지 중 `SIGTERM` → 종료 소요시간 측정(1초 이내 검증)

**하니스 계약(서버 ↔ 테스트)**
- 부모(테스트): `fork`+`execv`로 서버를 포트 `0`으로 실행 → 자식 stdout pipe에서 `"PORT %u"` 파싱 → 그 포트로 `connect_with_retry`(10ms×최대 50회).
- 자식(서버): `bind(0)` → OS 임시 포트 → `local_port()`로 실제 포트 조회 → `PORT <n>` 출력.

---

## 2. 트러블슈팅

### 2-1. 서버 측 포트 통지 계약 부재 — 🔴 통합 테스트 end-to-end 차단(설계)
- **증상**: 통합 하니스는 자식 stdout에서 `"PORT %u"`(`echo_client.cpp:90`)를 읽어 포트를 얻는데, 서버는 해당 포트를 알릴 방법이 없었음. 이대로면 `start_server`의 `read` 루프가 응답을 못 받아 무한 블록(해당 시나리오엔 ctest `TIMEOUT`도 없음).
- **원인**: `src/` 전체에 stdout 출력(`printf`/`cout`/`write(1)`)이 전무했고(`grep` 확인), `getsockname`도 없어 `bind(0)`로 OS가 할당한 임시 포트를 되읽을 수단이 없었음. 즉 하니스만 작성되고 짝이 되는 서버 계약이 비어 있던 상태.
- **해결**: `Socket::local_port()` 추가(`getsockname(2)` → `ntohs(sin_port)`), `main`에서 `listen()` 직후 `std::printf("PORT %u\n", socket.local_port()); std::fflush(stdout);` 출력. 포트 인자(`argv[1]`)도 파싱해 테스트가 `0`을 주입할 수 있게 함.
- **교훈**: 자식 stdout이 pipe로 리다이렉트되면 **full-buffered**가 되어, `fflush`(또는 `setvbuf` line/no-buffer) 없이는 종료 전까지 한 줄도 부모에 도달하지 않는다. tty에서만 line-buffered라는 점이 테스트 hang의 흔한 원인.

### 2-2. `getsockname(fd, ...)` — 멤버 변수 대신 접근자 이름 전달 — 🔴 빌드 차단
- **증상**: Ubuntu 22.04 `builder` 스테이지 빌드 실패. `sockets`/`http_server` 두 타깃 모두 `socket.cpp.o`에서 컴파일 에러.
- **원인**: `local_port()`에서 `::getsockname(fd, ...)`로 작성 — `fd`는 멤버 변수(`fd_`)가 아니라 멤버 **함수** `int fd() const`. 함수명을 값으로 사용해 "invalid use of non-static member function" 발생. gcc가 선언부 `socket.h:27 int fd() const { return fd_; }`를 가리킴.
- **해결**: `fd` → `fd_`로 수정(멤버 변수 직접 사용). 이후 재빌드 성공.
- **교훈**: 같은 이름의 멤버 변수(`fd_`)와 접근자(`fd()`)가 공존할 때 괄호 누락은 컴파일 단계에서 잡힌다. 빌드는 반드시 Linux(Docker)에서 확인 — epoll 의존으로 macOS 네이티브 빌드는 불가.

---

## 3. 빌드 · 검증

> epoll 의존으로 macOS 호스트 네이티브 빌드 불가. 빌드·테스트 모두 Docker(Ubuntu 22.04)에서 수행.

### 빌드 (`builder` 스테이지)

```bash
docker build --target builder -t http-server-build .
```

```
[ 84%] Built target tests
[100%] Built target gmock_main
# 컴파일·링크 에러 0 (2-2 수정 후). EXIT=0
# 경고 1건(비차단): event_loop.cpp:114 enum/int 혼용 [-Wextra]
```

### ctest (빌드 이미지 내 build 트리)

```bash
docker run --rm http-server-build sh -c "cd /src/build && ctest --output-on-failure"
```

```
 1/11 SocketTest.ConstructorCreatesValidFd ......  Passed   0.00s
 ...
 6/11 SocketTest.BindAndListenSucceed ...........  Passed   0.00s
 7/11 integration.single_echo ..................  Passed   0.00s
 8/11 integration.concurrent_echo ..............  Passed   0.00s
 9/11 integration.large_payload ................  Passed   0.01s
10/11 integration.idle_cpu .....................  Passed   2.01s
11/11 integration.signal_shutdown ..............  Passed   0.00s

100% tests passed, 0 tests failed out of 11
Total Test time (real) = 2.03 sec
```

- `idle_cpu` 2.01s: 2초 idle 동안 CPU 사용이 임계값(0.2s) 미만 → busy-spin 회귀 없음 실증(`/proc` 존재하는 Docker라 정상 측정).
- `signal_shutdown` 0.00s: active 연결 상태에서도 즉시 종료 → self-pipe wakeup(6/27 작업) 효과 재확인.

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | echo 시나리오 `TIMEOUT` | `single_echo`/`concurrent_echo`/`large_payload`에 ctest `TIMEOUT` 미설정 — 서버가 멈추면 빠르게 실패하지 못하고 기본값까지 매달림 |
| 🟡 | `idle_cpu` skip 처리 | `set_tests_properties(integration.idle_cpu PROPERTIES SKIP_RETURN_CODE 77)` 추가 — `/proc` 없는 환경에서 fail이 아닌 skip으로 |
| ⚪ | `-Wextra` 경고 정리 | `event_loop.cpp:114` `EPOLLIN | (want ? EPOLLOUT : 0)` enum/int 혼용 경고 제거 |
| ⚪ | HTTP 파싱 도입 | 현재는 raw echo. 요청 파싱 → 응답 생성 단계로 확장 |

---

## 5. 핵심 교훈 요약

1. pipe로 리다이렉트된 자식 stdout은 full-buffered → 부모에 즉시 전달하려면 `fflush`(또는 `setvbuf`)가 필수. 누락 시 부모 `read`가 무한 블록된다.
2. `bind(0)`로 OS가 고른 임시 포트는 `getsockname(2)`로만 되읽을 수 있다 — 테스트가 포트를 고정하지 않아도 되게 하는 핵심.
3. 멤버 변수(`fd_`)와 접근자(`fd()`) 이름이 비슷하면 괄호 누락 실수가 쉽다. epoll 의존 프로젝트는 빌드 검증을 반드시 Linux(Docker)에서.
