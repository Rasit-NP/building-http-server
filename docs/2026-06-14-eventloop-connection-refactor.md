# EventLoop / Connection 클래스 분리 — 작업 & 트러블슈팅 로그

- **날짜**: 2026-06-14
- **작업**: `main.cpp`에 인라인으로 있던 epoll 이벤트 루프를 `EventLoop` 클래스로, client 연결 상태를 `Connection` 클래스로 분리. client 소유를 `std::unordered_map<int, std::unique_ptr<Connection>>`로 전환
- **결과**: gcc:12 컨테이너 빌드 성공(경고 0), `ctest` 6/6 통과, 런타임 echo 검증 통과

---

## 1. 작업 개요

| 항목 | Before | After |
|------|--------|-------|
| 이벤트 루프 | `main.cpp` 안 인라인 `while(epoll_wait)` 루프 | `EventLoop::run()` 으로 분리 |
| client 상태 | `std::unordered_map<int, Socket>` (fd만 보유) | `Connection` 객체(`Socket` + `read_buf`/`write_buf`) |
| client 소유 | 맵이 `Socket` 직접 보유 | `std::unordered_map<int, std::unique_ptr<Connection>>` |
| epoll 이벤트 식별 | `events[i].data.fd` (fd로 맵 조회) | `events[i].data.ptr` 에 `Connection*` 직접 저장(listen은 `nullptr`) |
| client I/O | `main` 안 raw `::read`/`::write` | `Connection::on_readable()` 로 캡슐화 |

- listening socket은 `data.ptr == nullptr`로 구분하여 `accept_new()` 분기.
- epoll fd는 `EventLoop` 소멸자에서 `::close(epoll_fd)`로 정리.

---

## 2. 트러블슈팅

### 2-1. `EventLoop(socket, ...)` 호출에서 `Socket` 복사 시도 → 컴파일 실패 🔴
- **증상**: `main.cpp`의 `EventLoop event_loop = EventLoop(socket, epoll_fd);` 에서
  `error: use of deleted function 'Socket::Socket(const Socket&)'`.
- **원인**: `Socket`은 복사 생성자/대입이 `= delete`(이동 전용)인데, `EventLoop` 생성자가 `Socket`을 **값(by value)** 으로 받음. 호출부에서 **lvalue** `socket`을 넘겨 복사 생성자가 선택됨.
- **해결**: 호출부를 `EventLoop(std::move(socket), epoll_fd)` 로 변경 → 생성자 초기화 `listen_socket(std::move(listen_socket))`가 이동 생성자로 연결됨.
- **교훈**: 이동 전용(move-only) 타입을 값 매개변수로 받는 함수에는 반드시 rvalue(`std::move`)로 넘긴다. lvalue를 넘기면 컴파일러가 복사 경로를 선택해 삭제된 함수에 걸린다.

### 2-2. move 이후 `socket.fd()`로 epoll 등록 → fd 무효(`EBADF`) 위험 🔴
- **증상**: 2-1을 `std::move`로만 고칠 경우, move된 `socket`은 `fd_ == -1`이 되어 그 뒤의 `epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket.fd(), ...)`가 `-1` fd로 등록을 시도(`EBADF`)하게 됨.
- **원인**: 소유권 이전(`std::move`)과 listen fd의 epoll 등록 사이의 **순서 의존성**. 이전이 먼저 일어나면 원본 객체는 fd를 잃는다.
- **해결**: listen socket의 `epoll_ctl(EPOLL_CTL_ADD)` 등록을 **`std::move`보다 앞**으로 재배치. 등록 시점에는 `socket`이 아직 fd를 소유하므로 유효한 fd로 등록되고, 그 후 `EventLoop`로 이관.
- **교훈**: move-only 자원은 "마지막으로 fd가 필요한 지점 → 그 다음에 소유권 이전" 순서를 지킨다. move는 원본을 비우는 부수효과가 있음을 코드 흐름상 항상 의식한다.

### 2-3. `event_loop.h`에서 `::close` 미선언으로 컴파일 실패 🔴
- **증상**: gcc:12에서 `event_loop.cpp` 컴파일 시
  `error: '::close' has not been declared; did you mean 'pclose'?` (`event_loop.h:23`, `~EventLoop()` 내부). 이어서 링크 단계에서 `undefined reference to 'EventLoop::run(...)'`(2차 증상).
- **원인**: 소멸자에서 `::close(epoll_fd)`를 호출하나 `event_loop.h`/`event_loop.cpp` 어디에서도 `<unistd.h>`를 include하지 않음. `close()`는 `<unistd.h>` 선언으로, 다른 툴체인에서는 transitive include로 우연히 통과했을 뿐 gcc:12에서는 드러남.
- **해결**: `event_loop.h`에 `# include <unistd.h>` 추가.
- **교훈**: 사용하는 심볼의 헤더는 직접 include한다(IWYU). transitive include 의존은 툴체인이 바뀌면 깨진다.

---

## 3. 빌드 · 검증

epoll은 Linux 전용이라 macOS 로컬 빌드 불가 → `gcc:12` 컨테이너에서 빌드·테스트·런타임 검증.

```bash
# 전체 빌드 + 단위 테스트
docker run --rm -v "$PWD":/src -w /src gcc:12 bash -c '
  apt-get update -qq && apt-get install -y -qq cmake
  cmake -S . -B /tmp/build -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/build -j
  ctest --test-dir /tmp/build --output-on-failure'
```

```
CONFIGURE OK
BUILD OK
# ctest: 100% tests passed, 0 tests failed out of 6
1/6 SocketTest.ConstructorCreatesValidFd ..... Passed
2/6 SocketTest.DestructorClosesFd ............ Passed
3/6 SocketTest.MoveConstructorResetsSource ... Passed
4/6 SocketTest.MoveAssignmentResetsSource .... Passed
5/6 SocketTest.MoveTransfersOwnership ........ Passed
6/6 SocketTest.BindAndListenSucceed .......... Passed
```

```bash
# 런타임 echo 검증 (서버 기동 후 /dev/tcp로 왕복)
g++ -std=c++17 -Iinclude src/*.cpp -o /tmp/http_server
/tmp/http_server &
printf "hello epoll" | bash -c 'exec 3<>/dev/tcp/127.0.0.1/8080; cat >&3; head -c 11 <&3'
```

```
SENT: hello epoll
RECV: hello epoll
```

- `main.cpp` 단독 컴파일도 경고/오류 0으로 통과 → 2-1/2-2 수정이 정확함을 확인.

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | partial write 처리 | `Connection::on_readable()`이 write를 1회 호출로 가정. 일부만 써지면 잔여가 `write_buf`에 남으나 `EPOLLOUT` 미등록이라 재전송 경로 없음. write `EAGAIN`(`<0`) 처리도 없음 |
| 🟡 | `Socket` 멤버 미사용 | `EventLoop`/`Connection`이 raw `::accept`/`::read`/`::write`를 직접 호출. `Socket`의 동명 멤버로 일원화 검토 |
| ⚪ | edge-triggered 전환 | `EPOLLET` 도입 시 소진 루프 구조 재설계 |
| ⚪ | HTTP 파싱 | 현재 echo. 요청 라인/헤더 파싱 및 응답 생성 |

---

## 5. 핵심 교훈 요약

1. move-only 타입은 (a) 값 매개변수에 lvalue를 넘기면 삭제된 복사 경로에 걸리고, (b) `std::move` 후 원본은 비므로 "마지막 사용 → 이후 이전" 순서를 지켜야 한다.
2. 사용하는 심볼의 헤더는 직접 include한다 — transitive include 의존은 툴체인 교체 시 깨진다(gcc:12에서 `::close` 노출).
3. epoll 같은 플랫폼 전용 코드는 macOS 로컬이 아닌 Linux 컨테이너(gcc:12)에서 실제 빌드·실행으로 검증한다.
