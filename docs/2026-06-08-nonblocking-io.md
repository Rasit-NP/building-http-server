# non-blocking I/O 전환 + 종료 플래그 atomic 화 — 작업 & 트러블슈팅 로그

- **날짜**: 2026-06-08
- **작업**: listening/client socket을 `O_NONBLOCK`으로 전환, `handle_client()` 분리, 종료 플래그를 `std::atomic<bool>`로 교체
- **결과**: 작업 중 빌드 차단(🔴) 1건 발견·수정 → 최종 `cmake --build` 성공 · `ctest` 6/6 통과 · echo 동작 확인. 다만 non-blocking 단독 적용으로 **idle CPU 100% busy-spin**(🔴)을 실측 — Day 3 `epoll` EventLoop에서 해소 예정

---

## 1. 작업 개요

| 항목 | Before | After |
|------|--------|-------|
| 종료 플래그 | `volatile sig_atomic_t g_should_stop` | `std::atomic<bool> g_should_stop{false}` + `static_assert(is_always_lock_free)` |
| client 처리 | `main()` 내부 inline 루프 | `handle_client(Socket&)` 함수로 분리 |
| socket 모드 | blocking (기본) | listening·client 모두 `set_nonblocking()` 적용 |
| `Socket::set_nonblocking` | 없음 | `fcntl(F_GETFL)`→`F_SETFL`로 `O_NONBLOCK` 부여 (멤버 `fd_` 대상) |
| `Socket::accept()` 반환 | `EINTR`에서만 `nullopt` | `EAGAIN`/`EWOULDBLOCK`/`EINTR`에서 `nullopt` (non-blocking 호환) |

### 설계 메모

- **`std::atomic<bool>` + `is_always_lock_free`**: signal handler에서 플래그를 쓰므로 lock-free를 컴파일 타임에 강제했다. lock 기반 atomic은 async-signal-safe하지 않다. 폴링은 `load(std::memory_order_relaxed)`로 충분.
- **`set_nonblocking()` 인자 제거**: 초기엔 `set_nonblocking(int fd)`로 외부 fd를 받았으나, 멤버 함수가 자기 `fd_`를 두고 외부 fd를 받는 형태(`client.set_nonblocking(client.fd())`)가 어색해 인자 없이 `fd_`에 작용하도록 정리.
- **`accept()` 래퍼 유지**: non-blocking 전환 초기엔 `main`이 raw `::accept()`를 직접 호출해 추상화가 둘로 갈라졌었다. `Socket::accept()`가 `EAGAIN`에서도 `nullopt`를 반환하도록 확장해 래퍼를 계속 쓰도록 일원화.

---

## 2. 트러블슈팅

### 2-1. `main()` accept 루프 컴파일 차단 + 논리 오류 🔴
- **증상**: 다음 형태로 작성한 루프가 컴파일되지 않음.
  ```cpp
  Socket client;            // (a)
  client.accept();          // (b)
  if (client == std::nullopt) {   // (c)
      continue;
  }
  ```
  ```
  main.cpp:52:20: error: invalid operands to binary expression ('Socket' and 'const nullopt_t')
     52 |         if (client == std::nullopt) {
        |             ~~~~~~ ^  ~~~~~~~~~~~~
  ```
- **원인**: 세 겹의 문제가 겹쳤다.
  - (c) `client`는 `Socket`이지 `std::optional`이 아니라 `std::nullopt`와 비교 불가 — `Socket`엔 `operator==(nullopt_t)`가 없음 → **컴파일 차단**.
  - (a) `Socket client;`는 default 생성자라 `socket(AF_INET, ...)`로 **새 fd를 생성**한다. bind/listen 한 것은 `main`의 `socket`인데 accept 대상이 엉뚱해지고, 매 반복마다 새 fd를 만들어 누수.
  - (b) `accept()`는 수락된 연결을 `std::optional<Socket>`로 **반환**하는데 그 값을 버렸다. 설령 (a)(c)를 고쳐도 bind/listen 안 한 소켓에 accept → `EINVAL` → `EAGAIN`/`EINTR`이 아니므로 `Socket::accept()`가 `throw runtime_error("accept() failed")` → 첫 반복에서 예외 종료.
- **해결**: listening `socket`에 accept하고 반환 optional을 받도록 수정.
  ```cpp
  std::optional<Socket> client = socket.accept();
  if (client == std::nullopt) continue;   // optional 비교는 합법
  client->set_nonblocking();
  handle_client(*client);                 // Socket은 move-only → 참조 전달
  ```
- **교훈**: optional을 반환하는 API는 반환값을 받아야 의미가 있다. `Socket`은 move-only RAII 타입이라 `Socket client;`(새 소켓 생성)와 `auto client = x.accept();`(소유권 수령)를 혼동하면 엉뚱한 fd를 조작하게 된다.

### 2-2. non-blocking 단독 적용으로 idle CPU 100% busy-spin 🔴
- **증상**: 연결이 하나도 없는 idle 상태에서 서버가 CPU를 100% 점유 (실측, §3-C).
  ```
  41121  95.1   0:01.01
  41121 100.0   0:03.01
  ```
- **원인**: listening socket이 `O_NONBLOCK`이라 `accept()`가 즉시 `EAGAIN`을 반환하고, 루프가 쉬지 않고 회전한다(busy-poll). blocking이던 이전엔 커널이 스레드를 재웠는데 그 대기가 사라졌다. `handle_client()` 내부도 client가 데이터를 안 보내면 `EAGAIN`에서 spin.
- **해결**: (현 단계 미해결, 향후 과제) `epoll`/`poll`/`select` 기반 I/O multiplexing 도입 시 `epoll_wait()`가 이벤트 전까지 블록하여 idle CPU가 ~0%로 떨어진다. → Day 3 EventLoop.
- **교훈**: non-blocking은 그 자체가 목적이 아니라 **I/O multiplexing의 전제 조건**이다. epoll 없이 non-blocking만 적용하면 blocking 대비 오히려 CPU 사용이 악화된다. 이번 변경은 Day 3와 짝이 되어야 의미를 갖는 디딤돌이다.

---

## 3. 빌드 · 검증

### (A) 수정 전 — 컴파일 차단 재현 (§2-1)
```bash
cmake --build build
```
```
main.cpp:52:20: error: invalid operands to binary expression ('Socket' and 'const nullopt_t')
make[2]: *** [src/CMakeFiles/http_server.dir/main.cpp.o] Error 1
make: *** [all] Error 2
```

### (B) 수정 후 — 빌드 · 테스트 통과
```bash
cmake --build build
ctest --test-dir build --output-on-failure
```
```
[ 73%] Built target http_server
[100%] Built target tests

1/6 Test #1: SocketTest.ConstructorCreatesValidFd .....   Passed
2/6 Test #2: SocketTest.DestructorClosesFd ............   Passed
3/6 Test #3: SocketTest.MoveConstructorResetsSource ...   Passed
4/6 Test #4: SocketTest.MoveAssignmentResetsSource ....   Passed
5/6 Test #5: SocketTest.MoveTransfersOwnership ........   Passed
6/6 Test #6: SocketTest.BindAndListenSucceed ..........   Passed
100% tests passed, 0 tests failed out of 6
```

### (C) 런타임 검증 — echo · idle CPU · 종료
```bash
./build/src/http_server & SRV=$!
sleep 1
ps -p $SRV -o pid,%cpu,time | tail -1   # idle CPU
printf 'hello-echo' | nc -w1 127.0.0.1 8080   # echo
kill -INT $SRV                          # SIGINT 종료
```
```
=== idle CPU sample 1 ===  41121  95.1   0:01.01
=== idle CPU sample 2 ===  41121 100.0   0:03.01     ← busy-spin (§2-2)
=== echo test ===          hello-echo               ← echo 정상
exited on SIGINT                                     ← idle 종료 정상
```
- echo 정상 반향 확인.
- idle CPU 100% — §2-2 busy-spin 실측.
- idle 상태 `SIGINT` 종료 정상. **단, 활성 연결 중 종료는 미보장**(§4): `handle_client()`의 내부 `while(true)`가 `g_should_stop`를 확인하지 않아 연결 유지 중 `EAGAIN`/`EINTR`에서 `continue`만 한다.

> 주의: 첫 실행 시 이전 테스트의 잔여 `http_server`가 포트 8080을 잡고 있어 `bind() failed` 예외가 발생했다. `pkill -f http_server` + `lsof -ti:8080 | xargs kill` 로 정리 후 재측정. (`SO_REUSEADDR`는 `TIME_WAIT` 재바인딩용이지 살아있는 프로세스의 점유를 풀어주지는 않는다.)

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🔴 | busy-spin 제거 | `epoll` EventLoop 도입. `epoll_wait()` 블로킹으로 idle CPU 0%화 (§2-2) |
| 🟡 | 활성 연결 중 종료 | `handle_client()` 내부 루프에 `g_should_stop` 확인 추가, 또는 EventLoop 전환 시 구조적 해소 (epoll_wait `EINTR` 재확인 / `signalfd`) |
| 🟡 | `write()` 결과 처리 | `client.write(buf, n)` 반환값을 무시 중. non-blocking에서 부분 쓰기/`EAGAIN` 가능 → Connection 출력 버퍼 + `EPOLLOUT` 재등록 필요 |
| ⚪ | EventLoop/Connection 분리 | 책임 경계 분리. `Connection` 소유권을 `unordered_map<int, unique_ptr<Connection>>`로, `epoll_event.data.ptr` 수명·배치 내 use-after-free·`EPOLLHUP`/`EPOLLERR` 처리 주의 |

---

## 5. 핵심 교훈 요약

1. **non-blocking은 epoll/poll의 전제일 뿐 단독으로는 busy-spin을 부른다.** blocking accept가 주던 커널 대기를 잃어 idle CPU가 100%가 된다 — multiplexing과 반드시 짝지어야 한다.
2. **optional 반환 API는 반환값을 받아야 한다.** `Socket::accept()`의 `std::optional<Socket>`를 버리고 호출하면 수락된 연결이 즉시 close되고, move-only `Socket`의 default 생성(`Socket client;`)과 혼동하면 엉뚱한 fd를 조작한다.
3. **signal handler가 만지는 플래그는 lock-free atomic으로.** `std::atomic<bool>` + `static_assert(is_always_lock_free)`로 async-signal-safety를 컴파일 타임에 보장한다.
4. **종료 경로는 "어디서 막혀 있는가"를 본다.** idle 종료가 되어도 활성 연결 중 inner loop가 플래그를 안 보면 종료가 막힌다 — graceful shutdown은 모든 블로킹 지점에서 신호를 확인해야 완성된다.
