# calculator 제거 및 Socket 단위 테스트 도입 — 작업 & 트러블슈팅 로그

- **날짜**: 2026-06-07
- **작업**: 더미 `calculator` 모듈 제거, 테스트를 RAII `Socket` 클래스 단위 테스트로 전환
- **결과**: 빌드 차단 이슈(🔴) 2건 발견·수정 완료 — ① `sockets` 링크 누락, ② `sockets` include 경로 누락. 작성 환경엔 cmake 미설치라 `clang++`로 링크 단계를 재현했고, 이후 사용자 환경의 cmake 빌드에서 ②가 드러나 수정함(아래 §3).

---

## 1. 작업 개요

| 항목 | Before | After |
|------|--------|-------|
| `src/calculator.{cpp,h}` | `add()`, `divide()` 더미 모듈 존재 | 삭제 |
| `src/CMakeLists.txt` | `calculator` static library 정의 + `http_server`에 링크 | library 정의·링크 제거, `http_server`는 `main.cpp`+`socket.cpp`만 |
| `tests/CMakeLists.txt` | `calculator` 링크, include 경로 없음 | `sockets` library(`../src/socket.cpp`) 정의·링크, `include/` 경로 추가 |
| `tests/tests.cpp` | `CalculatorTest` 2건 (`add`) | `SocketTest` 6건 (생성/소멸/move/bind·listen) |

### 추가된 테스트 (`tests/tests.cpp`)

| 테스트 | 검증 내용 |
|--------|-----------|
| `ConstructorCreatesValidFd` | 생성 시 유효한 `fd`(≥0) 확보 및 열림 상태 |
| `DestructorClosesFd` | 소멸자가 `fd`를 close (RAII) |
| `MoveConstructorResetsSource` | move 생성 후 원본 `fd` == -1 |
| `MoveAssignmentResetsSource` | move 대입 후 원본 `fd` == -1 |
| `MoveTransfersOwnership` | 소유권 이전 + 이중 close 방지 |
| `BindAndListenSucceed` | `bind(0)`(포트 자동 할당) + `listen()` 예외 없음 |

- fd 생존 확인은 `is_fd_open()` 헬퍼(`::fcntl(fd, F_GETFD) != -1`)로 수행.

---

## 2. 트러블슈팅

### 2-1. `tests` 타깃이 `sockets` library를 링크하지 않아 undefined symbols 🔴
- **증상**: `tests/CMakeLists.txt`가 `add_library(sockets ../src/socket.cpp)`로 라이브러리를 **정의만** 하고 `target_link_libraries(tests ...)` 목록에는 추가하지 않은 상태였다. 이 구성으로 링크하면 `Socket`의 비-inline 멤버 전부가 undefined symbol로 떨어진다.
  ```
  Undefined symbols for architecture arm64:
    "Socket::bind(unsigned short)", referenced from: _main
    "Socket::listen(int)", referenced from: _main
    "Socket::Socket(Socket&&)", referenced from: _main
    "Socket::Socket()", referenced from: _main
    "Socket::~Socket()", referenced from: _main
    "Socket::operator=(Socket&&)", referenced from: _main
  ld: symbol(s) not found for architecture arm64
  ```
- **원인**: `add_library`는 빌드 대상을 *선언*할 뿐, 해당 심볼을 실행 파일에 넣으려면 `target_link_libraries`로 *링크*해야 한다. 두 단계가 분리돼 있는데 링크 단계가 누락됐다. (`Socket::fd()`만 헤더 inline이라 링크 에러에서 빠짐 — 나머지는 `socket.cpp`에 정의)
- **해결**: `target_link_libraries(tests PRIVATE GTest::gtest_main sockets)`로 `sockets`를 추가. (사용자가 직접 수정 완료, 현재 파일에 반영됨)
- **교훈**: `add_library`/`add_executable`로 타깃을 만든 뒤 `target_link_libraries` 연결까지 한 쌍으로 확인한다. 정적 라이브러리는 "정의 ≠ 링크"이며, 미링크 시 컴파일은 통과하고 **링크 단계**에서만 터지므로 끝까지 빌드해 봐야 드러난다.

### 2-2. `sockets` library 컴파일 시 `socket.h`를 찾지 못함 🔴
- **증상**: 2-1 수정 후 `cmake --build build/tests` 실행 시 `sockets` 라이브러리 컴파일 단계에서 헤더를 못 찾고 중단.
  ```
  [ 12%] Building CXX object tests/CMakeFiles/sockets.dir/__/src/socket.cpp.o
  /Users/rasit/.../src/socket.cpp:1:11: fatal error: 'socket.h' file not found
      1 | # include "socket.h"
        |           ^~~~~~~~~~
  1 error generated.
  make[2]: *** [tests/CMakeFiles/sockets.dir/__/src/socket.cpp.o] Error 1
  make: *** [all] Error 2
  ```
- **원인**: `socket.cpp`는 `#include "socket.h"`를 하고 `socket.h`는 `include/`에 있다. `src/CMakeLists.txt`의 `http_server`에는 `target_include_directories(... ${CMAKE_SOURCE_DIR}/include)`가 있어 정상이지만, `tests/CMakeLists.txt`의 `add_library(sockets ...)`에는 include 경로가 없었다. 같은 파일의 `target_include_directories(tests PRIVATE ...)`는 **`tests` 실행 파일 전용**이라 `sockets` 라이브러리 컴파일에는 적용되지 않는다.
- **해결**: `sockets` 타깃에 include 경로를 부여.
  ```cmake
  add_library(sockets ../src/socket.cpp)
  target_include_directories(sockets PUBLIC ${CMAKE_SOURCE_DIR}/include)
  ```
  `PUBLIC`으로 주면 `sockets` 자신(socket.cpp)과 이를 링크하는 `tests` 모두 경로를 상속받으므로, 기존 `target_include_directories(tests PRIVATE ${CMAKE_SOURCE_DIR}/include)`는 중복이 된다. (사용자가 직접 수정 진행)
- **교훈**: include 경로는 **타깃별로 분리**된다. `target_include_directories(tests ...)`는 `tests`에만 적용되고 같은 디렉토리의 다른 타깃(`sockets`)에는 전파되지 않는다. 여러 타깃이 같은 헤더를 쓰면 헤더를 *소유*한 타깃에 `PUBLIC`/`INTERFACE`로 걸어 의존 타깃이 상속받게 하는 편이 견고하다. (직전 §3의 `clang++ -Iinclude` 재현은 경로를 전역으로 줬기에 이 타깃별 분리 문제가 드러나지 않았다.)

---

## 3. 빌드 · 검증

> 이 환경에는 `cmake`/`ninja`가 설치돼 있지 않아(`/usr/bin/{g++,clang++,make}`만 존재) gtest를 FetchContent로 받는 정식 `cmake` 빌드 및 `ctest`는 **실행하지 못했다**. 대신 문제의 핵심인 **링크 단계**를 `clang++`로 동일하게 재현했다. tests가 호출하는 `Socket` 멤버(생성/move/`bind`/`listen`/소멸)를 그대로 사용하는 최소 드라이버를 만들어 검증.

### (A) `socket.cpp` 미링크 = 수정 전 `tests/CMakeLists.txt` 구성
```bash
clang++ -std=c++17 -Iinclude link_probe.cpp -o probe_nolink
```
```
Undefined symbols for architecture arm64:  (§2-1 참조)
ld: symbol(s) not found for architecture arm64
clang++: error: linker command failed with exit code 1
exit=1   ← 링크 실패 (문제 재현)
```

### (B) `socket.cpp` 링크 = 수정 후(`sockets` 링크) 구성
```bash
clang++ -std=c++17 -Iinclude link_probe.cpp src/socket.cpp -o probe_link
./probe_link
```
```
exit=0       ← 링크 성공
run exit=0   ← 실행 정상
```

→ `sockets` 링크 추가로 undefined symbols가 해소됨을 확인. (`link_probe.cpp`는 검증용 임시 파일로 작업 후 삭제)

### (C) 사용자 환경 cmake 빌드 — include 경로 누락 노출 (§2-2)
2-1 수정 후 cmake가 설치된 사용자 환경에서 빌드하자, `clang++ -Iinclude` 재현에선 가려졌던 타깃별 include 경로 문제가 드러났다.
```bash
cmake --build build/tests
```
```
[ 12%] Building CXX object tests/CMakeFiles/sockets.dir/__/src/socket.cpp.o
/Users/rasit/.../src/socket.cpp:1:11: fatal error: 'socket.h' file not found
make: *** [all] Error 2
```
→ `target_include_directories(sockets PUBLIC ${CMAKE_SOURCE_DIR}/include)` 추가로 해결(§2-2).

### 남은 검증
②까지 수정 후 빌드·테스트 통과 확인 권장:
```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | gtest 포함 정식 빌드 | ② 수정 후 `ctest`로 `SocketTest` 6건 실제 통과 확인 |
| ⚪ | 테스트 커버리지 확장 | `accept()`, `read()`, `write()` 경로에 대한 테스트 추가 |
| ⚪ | `explicit Socket(int fd)` 테스트 | fd 주입 생성자 동작 검증 |

---

## 5. 핵심 교훈 요약

1. CMake에서 `add_library`(정의)와 `target_link_libraries`(링크)는 별개 단계다 — 라이브러리를 만들었어도 링크하지 않으면 **링크 시점**에 undefined symbol로 터진다.
2. inline 멤버(`fd()`)는 링크 에러에 나타나지 않으므로, undefined symbol 목록만 보고 "일부만 빠졌다"고 오해하지 말 것.
3. `target_include_directories`는 **타깃별로 적용**된다 — 한 타깃에 준 경로가 같은 디렉토리의 다른 타깃에 전파되지 않는다. 헤더 소유 타깃에 `PUBLIC`으로 걸어 의존 타깃이 상속받게 하라.
4. 검증 환경이 실제 빌드 구성과 다르면(여기선 `clang++ -Iinclude` 전역 경로 vs cmake 타깃별 경로) 일부 문제가 가려질 수 있다 — 가능하면 실제 빌드 시스템으로 끝까지 확인한다.
