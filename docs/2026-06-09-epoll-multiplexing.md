# epoll 다중 연결 처리 & client Socket 소유권 — 작업 & 트러블슈팅 로그

- **날짜**: 2026-06-09
- **작업**: `epoll` 기반 이벤트 루프로 다중 연결 echo 처리 전환, accept한 client `Socket`을 `std::unordered_map<int, Socket>`로 소유
- **결과**: gcc:12 컨테이너 빌드 성공(경고 0), `ctest` 6/6 통과, 런타임 echo 검증 통과

---

## 1. 작업 개요

| 항목 | Before | After |
|------|--------|-------|
| I/O 모델 | nonblocking + 단일 연결 처리 | `epoll`(level-triggered) 이벤트 루프로 다중 연결 |
| listening socket | epoll 미사용 | `epoll_create1(EPOLL_CLOEXEC)` + `EPOLL_CTL_ADD` 등록 |
| client fd 소유 | (없음) | `std::unordered_map<int, Socket> clients` 가 연결 수명 동안 소유 |
| client I/O | raw `::read`/`::write`/`::close` | `clients.at(fd)` 의 `Socket::read()`/`Socket::write()`, 종료 시 `erase()`(RAII close) |

- epfd 자체도 `Socket(epfd)`로 감싸 종료 시 RAII 정리.
- 소유권 원칙: **연결이 살아있는 동안 `Socket` 객체도 살아있어야** epoll/연결 수명과 fd 수명이 일치한다. 컨테이너가 그 수명을 보유한다.

---

## 2. 트러블슈팅

### 2-1. `MAX_EVENTS` 미선언으로 컴파일 실패 🔴
- **증상**: `cmake --build` 시
  `error: 'MAX_EVENTS' was not declared in this scope` (`main.cpp:64`, `main.cpp:67`), 이어서 `'events' was not declared in this scope`.
- **원인**: `epoll_wait`에 넘길 이벤트 버퍼 크기 상수를 사용만 하고 정의하지 않음.
- **해결**: 파일 상단에 `constexpr int MAX_EVENTS = 64;` 추가.
- **교훈**: epoll 버퍼 크기 같은 매직 넘버는 사용처와 분리된 상수로 먼저 선언한다.

### 2-2. accept한 client `Socket`이 즉시 소멸 → 연결이 바로 끊김 🔴
- **증상**: accept는 되지만 클라이언트가 데이터를 보내도 EPOLLIN 이벤트가 오지 않고 연결이 끊김(echo 미동작).
- **원인**: `std::optional<Socket> client`를 `if (fd == listen)` 블록 **지역 변수**로 두어, epoll에 등록(`EPOLL_CTL_ADD`)한 직후 블록을 벗어나며 `~Socket()`이 해당 fd를 `close()`. 닫힌 fd는 epoll에서 자동 제거되어 이벤트가 발생하지 않음.
- **해결**: accept한 `Socket`을 `clients.emplace(client->fd(), std::move(*client))`로 컨테이너에 이관(move)하여 연결 수명만큼 보존.
- **교훈**: RAII 핸들은 "누가, 언제까지 소유하는가"를 명시적으로 정해야 한다. 이벤트 루프에서 fd 소유권은 연결 수명과 일치하는 컨테이너가 갖는다.

### 2-3. `clients` 맵을 `for` 루프 안에 선언 🔴
- **증상**: 컴파일은 되나, (a) accept 직후 연결이 끊기고 (b) 데이터 이벤트 처리 시 `clients.at(fd)`가 `std::out_of_range`를 던져 `std::terminate`로 프로세스 종료.
- **원인**: `std::unordered_map<int, Socket> clients;`를 이벤트 처리 `for (int i...)` 본문 안에 두어, 매 iteration마다 **빈 맵이 새로 생성·소멸**. (a) iteration 종료 시 맵 소멸로 보관 중인 `Socket`이 close됨, (b) else 브랜치 진입 시 맵이 비어 있어 `at(fd)` 실패.
- **해결**: 선언을 `while` 루프 **바깥**(이벤트 버퍼 옆)으로 이동하여 모든 이벤트/iteration에 걸쳐 맵이 유지되도록 함.
- **교훈**: 상태(소유권)를 담는 컨테이너는 이벤트 루프 **바깥** 스코프에 둔다. 선언 위치 한 줄이 수명 전체를 좌우한다.

> 참고: client I/O를 raw `::write` 대신 `Socket::write()` 멤버로 전환하면서, 기존 `::write` 반환값 무시(`-Wunused-result`) 경고도 함께 사라짐.

---

## 3. 빌드 · 검증

epoll은 Linux 전용이라 macOS 로컬 빌드 불가 → `gcc:12` 컨테이너에서 빌드·테스트·런타임 검증.

```bash
# 빌드 + 단위 테스트
docker run --rm -v "$PWD":/src -w /src gcc:12 bash -c '
  apt-get update -qq && apt-get install -y -qq cmake
  cmake -S . -B /tmp/build -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/build -j
  ctest --test-dir /tmp/build --output-on-failure'
```

```
# 빌드: 경고/에러 0
[ 40%] Built target http_server
[ 80%] Built target tests
...
# ctest: 100% tests passed, 0 tests failed out of 6
1/6 SocketTest.ConstructorCreatesValidFd ..... Passed
2/6 SocketTest.DestructorClosesFd ............ Passed
3/6 SocketTest.MoveConstructorResetsSource ... Passed
4/6 SocketTest.MoveAssignmentResetsSource .... Passed
5/6 SocketTest.MoveTransfersOwnership ........ Passed
6/6 SocketTest.BindAndListenSucceed .......... Passed
```

```bash
# 런타임 echo 검증 (서버 기동 후 Python 클라이언트로 왕복)
/tmp/build/src/http_server &      # 컨테이너 내부
# conn1: "hello" 송신 → 수신, conn2: 한 연결에서 "first"/"second" 연속 왕복
```

```
# 결과
conn1 echo: b'hello'
conn2 round1: b'first' round2: b'second'
ALL ECHO TESTS PASSED
```

- 한 연결에서 연속 2회 왕복이 성공 → 2-2/2-3의 "accept 직후 끊김" 증상이 해소되었음을 확인.

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | partial write 처리 | `Socket::write()`가 1회 호출로 전량 전송을 가정. 큰 응답·`EAGAIN` 시 잔여 바이트 재전송 로직 필요 |
| 🟡 | `at(fd)` 방어 | 비정상 경로로 맵에 없는 fd 진입 시 throw. 조회 실패를 안전 무시하거나 로깅 후 정리 |
| ⚪ | edge-triggered 전환 | `EPOLLET` 도입 시 accept/read 루프(소진할 때까지 반복) 구조로 재설계 |
| ⚪ | HTTP 파싱 | 현재 echo. 실제 요청 라인/헤더 파싱 및 응답 생성 |

---

## 5. 핵심 교훈 요약

1. RAII 핸들의 소유권 수명은 자원의 논리적 수명(연결)과 일치시킨다 — 이벤트 루프에서는 컨테이너가 소유한다.
2. 상태 컨테이너의 **선언 스코프**가 정확성을 좌우한다. 루프 안/밖 한 줄 차이가 즉시 끊김·`std::terminate`로 이어진다.
3. epoll 같은 플랫폼 전용 코드는 macOS 로컬이 아닌 Linux 컨테이너(gcc:12)에서 실제 빌드·실행으로 검증한다.
