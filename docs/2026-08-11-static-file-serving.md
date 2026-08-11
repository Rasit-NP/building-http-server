# static-file-serving — 작업 & 트러블슈팅 로그

- **날짜**: 2026-08-11
- **작업**: 하드코딩 응답(`"Success"`)을 걷어내고 `StaticFileHandler`를 도입해 `public/` 아래 정적 파일을 서빙하도록 전환. path traversal 가드와 컨테이너 배포까지 포함
- **결과**: Ubuntu 22.04 컨테이너 전체 빌드 성공·`ctest` **34/34 통과**(2.03s)·신규 TU 경고 0·ASan/UBSan 리포트 0. `docker compose` 기동 후 브라우저 경로(`GET /index.html`) 200/151바이트 확인, traversal 3종 차단 확인

---

## 1. 작업 개요

07-22에 파서를 서버 흐름에 편입하고 08-02에 응답 **형식**을 `HttpResponse`로 분리한 데 이어, 이번에는 응답 **내용** 생성을 `Connection` 밖으로 뺐다. `Connection`은 이제 소켓 I/O와 파싱 상태만 다루고, 무엇을 응답할지는 `StaticFileHandler`가 결정한다.

| 항목 | Before | After |
|------|--------|-------|
| 응답 내용 | `connection.cpp`에서 `res.body = "Success"` 하드코딩 + request line `printf` 디버그 | `handler_.handle(req).serialize()` 위임 |
| 파일 읽기 | 없음 | `StaticFileHandler::handle()`이 `std::ifstream`(`std::ios::binary`) + `istreambuf_iterator`로 전체 읽기 |
| 경로 해석 | 없음 | `std::filesystem`으로 `root_`와 결합 후 `lexically_normal()`, `/`는 `/index.html`로 매핑 |
| 보안 | 없음 | 정규화된 full path가 base의 하위인지 prefix 검사(traversal 차단) |
| handler 소유 | — | `EventLoop`의 멤버(`StaticFileHandler handler_`), `Connection`은 `const&`로 참조 |
| 빌드 대상 | — | `src/CMakeLists.txt`에 `http/StaticFileHandler.cpp` 추가 |
| 배포 | 바이너리만 | `Dockerfile` runtime 스테이지에 `COPY public/ ./public/` |

### 설계 — handler는 `EventLoop`가 소유하고 `Connection`은 빌려 쓴다

`StaticFileHandler`는 `root_` 하나만 들고 있는 stateless에 가까운 객체라 연결마다 만들 이유가 없다. `EventLoop`가 값으로 소유해 루프 수명과 묶고, `Connection`은 `const&`로 참조만 한다. 이 계약을 어기면 2-2가 발생한다.

### 설계 — traversal 가드는 "정규화 후 prefix 검사"

`base / target` 결합 뒤 `lexically_normal()`로 `..`·`.`을 접고, 그 결과 문자열이 base로 시작하는지 + 경계가 `/`인지를 확인한다. 문자열 단계에서 `..`를 찾아 거르는 방식보다 우회가 어렵다. 부수 효과로 `//etc/passwd`처럼 absolute path가 base를 통째로 대체하는 경우(`std::filesystem` 결합 규칙)도 같은 검사에 걸린다.

---

## 2. 트러블슈팅

### 2-1. 🔴 `StaticFileHandler.cpp` CMake 미등록 — 링크 실패

- **증상**: `http_server` 링크 단계에서 중단.
  ```
  /usr/bin/ld: connection.cpp:(.text+0x94): undefined reference to
    `StaticFileHandler::handle(HttpRequest const&) const'
  collect2: error: ld returned 1 exit status
  ```
  `http_server` 타깃이 실패하므로 `integration.idle_cpu`를 포함한 `ctest` 전체가 함께 막혔다.
- **원인**: `src/CMakeLists.txt`의 `add_executable(http_server ...)` 소스 목록에 `http/StaticFileHandler.cpp`를 추가하지 않음. 헤더만 include돼 컴파일은 통과하고 링크에서만 드러났다.
- **해결**: 소스 목록에 `http/StaticFileHandler.cpp` 추가.
- **교훈**: 08-02에 `HttpResponse.cpp`를 추가할 때와 **동일한 재발**. 새 `.cpp` 작성과 CMake 등록은 한 묶음으로 처리한다. 헤더 include만으로는 컴파일이 통과하므로, 링크까지 돌려봐야 발견된다.

### 2-2. 🔴 `handler_` dangling reference — 모든 요청이 use-after-scope

- **증상**: ASan 빌드로 요청 1건을 보내자 즉시 리포트.
  ```
  ==26==ERROR: AddressSanitizer: stack-buffer-overflow
  READ of size 6 at 0xffffe8587d00
      #4 StaticFileHandler::handle(HttpRequest const&) const src/http/StaticFileHandler.cpp:13
      #5 Connection::on_readable()                           src/connection.cpp:17
  Address ... is located in stack of thread T0
    This frame has 5 object(s):
      [352, 4448) 'buf' (line 9) <== Memory access at offset 4464 overflows this variable
  ```
  ASan 없이는 정상 응답이 나와 증상이 드러나지 않았다.
- **원인**: `accept_new()` 안에서 `StaticFileHandler handler("public");`를 **지역 변수**로 만들고 `Connection`에 `const&`로 넘김. `accept_new()`가 반환되는 순간 파괴되는데 `Connection`은 그 참조를 계속 들고 있다. `READ of size 6`은 `root_`의 `"public"` 6바이트이며, 죽은 프레임을 `on_readable()`의 `buf[4096]`이 재사용한 상태라 리포트가 `buf` 오버플로 형태로 잡혔다.
- **해결**: `EventLoop`의 멤버 `StaticFileHandler handler_`로 올려 이벤트 루프 수명에 묶고, `accept_new()`는 그 멤버를 넘긴다.
- **교훈**: 참조 멤버는 "참조 대상이 나보다 오래 산다"는 **계약**이다. 참조를 걸기 전에 소유자를 먼저 정한다. 스택 재사용 탓에 우연히 동작할 수 있어, 이런 결함은 ASan 없이는 통과해 버린다.

### 2-3. 🔴 path traversal 가드가 세 형태로 잔존 — 세 라운드에 걸쳐 해소

세 결함이 한 조건식 안에 겹쳐 있었고, **하나씩 고칠 때마다 다른 형태로 살아남았다.**

- **증상 (1차)**: 가드가 한 번도 발동하지 않아 root 밖 파일이 그대로 노출.
  ```
  $ curl -s -i --path-as-is http://127.0.0.1:8080/../CLAUDE.md
  HTTP/1.1 200 OK
  Content-Length: 4203
  ```
- **증상 (2차)**: 비교를 고치자 이번엔 **정상 파일이 전부 거부**되고 traversal만 통과.
  ```
  GET /            -> HTTP/1.1 400 Bad Request
  GET /index.html  -> HTTP/1.1 400 Bad Request
  GET /../CLAUDE.md-> HTTP/1.1 200 OK  (4203 bytes)
  ```
- **원인**: 세 가지가 중첩.
  1. `full_str.compare(0, base_str.size(), base_str)`를 그대로 `&&`에 사용. `std::string::compare()`는 **일치할 때 `0`**(= false)을 반환하므로 조건이 정반대. 실측상 `inside`가 전 경로에서 `0`이었다.
  2. 변수명은 `inside`("root 안쪽")인데 `if (inside)`에서 거부 — 조건 의미가 반대.
  3. 블록 본문이 `// 404 응답 조립` 주석만 있는 빈 상태라, 값이 맞아도 아무 일도 일어나지 않음.
- **해결**: `compare(...) == 0`로 비교 방향을 바로잡고, `if (!inside)`로 조건을 뒤집고, 블록에서 실제 응답(`res.status_code = 400`)을 반환. 404 전환은 `reason_phrase()` 확장과 함께 후속으로 남김(코드 주석 `// 이후에 404로 수정`).
- **교훈**: `compare()`는 `bool`이 아니라 **3-way 정수**를 반환한다. `!` 하나로 뜻이 뒤집히는 자리다. 더 큰 교훈은 **결함이 서로를 가린다**는 점 — ①만 고치면 정상 파일이 전부 차단되고, ②③만 고치면 traversal이 열린다. 08-10 중복 `Content-Length` 건과 같은 구조이며, 조건식 하나에 검사·판정·처리가 뭉쳐 있으면 부분 수정이 오히려 악화시킨다. 정상 경로와 차단 경로를 **함께** 검증해야 어느 쪽으로 기울었는지 드러난다.

### 2-4. 🟡 참조 멤버 추가로 move assignment 암묵 삭제 — clang 경고

- **증상**: clang에서 신규 경고 1건(gcc는 침묵).
  ```
  include/connection.h:24:17: warning: explicitly defaulted move assignment operator
    is implicitly deleted [-Wdefaulted-function-deleted]
  note: ... because field 'handler_' is of reference type 'const StaticFileHandler &'
  ```
- **원인**: `const StaticFileHandler& handler_` 멤버를 추가하면 참조는 재대입이 불가능해 move assignment가 암묵 삭제되는데, 선언은 `= default`로 남아 있었다.
- **해결**: `Connection& operator=(Connection&&) = delete;`로 명시. `EventLoop`가 `std::unordered_map<int, std::unique_ptr<Connection>>`로 소유하므로 move assignment는 실제로 쓰이지 않는다.
- **교훈**: 참조·`const` 멤버 추가는 특수 멤버 함수의 가용성을 바꾼다. 그리고 **컴파일러마다 진단이 다르다** — 이 건은 gcc(CI) 빌드만 봤다면 놓쳤고, macOS clang 검사에서 잡혔다. 양쪽을 함께 돌린 것이 유효했다.

### 2-5. 🔴 컨테이너 이미지에 `public/` 부재 — 브라우저 빈 화면

- **증상**: `localhost:8080/index.html` 접속 시 아무것도 표시되지 않음. 서버는 살아 있고 응답도 정상 형식이었다.
  ```
  $ curl -s -i http://127.0.0.1:8080/index.html
  HTTP/1.1 200 OK
  Content-Length: 0
  ```
  ```
  $ docker exec building-http-server-server-1 sh -c 'pwd; ls -la'
  /app
  -rwxr-xr-x 1 root root 59304 http_server      ← 바이너리 하나뿐
  $ docker exec building-http-server-server-1 ls -la public
  ls: cannot access 'public': No such file or directory
  ```
- **원인**: `Dockerfile`의 builder 스테이지가 `CMakeLists.txt`/`src/`/`tests/`/`include/`만 `COPY`하고 `public/`을 복사하지 않음. runtime 스테이지도 `WORKDIR /app`에 바이너리만 배치. `root_`가 상대 경로 `"public"`이라 서버는 존재하지 않는 `/app/public/index.html`을 열려 했다. `.dockerignore`는 `public/`을 막고 있지 않으므로 원인은 `COPY` 누락뿐.
- **해결**: runtime 스테이지에 `COPY public/ ./public/` 추가. `CMD ["./http_server"]`의 CWD가 `/app`이므로 상대 경로가 맞아떨어진다. 반영에는 `docker compose up -d --build`로 **이미지 재빌드**가 필요하다(`restart`만으로는 반영되지 않음).
- **교훈**: **런타임 자산은 코드와 별개로 이미지에 넣어야 한다.** 컴파일·링크·테스트가 전부 통과해도 배포 산출물에 데이터가 빠지면 무의미하다. 그리고 이 증상이 끝까지 조용했던 이유는 파일을 못 열었는데도 `200 / Content-Length: 0`이 나갔기 때문이다 — 4장 🟡 1번(`ifstream` 실패 미검사)의 실제 비용이 여기서 드러났다. **오류를 오류로 보고하지 않는 코드는 무관한 층(배포 설정)의 문제를 자기 문제처럼 위장시킨다.**

---

## 3. 빌드 · 검증

### 3-1. 전체 빌드 · `ctest` (Ubuntu 22.04 컨테이너, CI 환경 기준)

```bash
docker run --rm -v "$PWD":/src -w /src ubuntu:22.04 bash -c '
  apt-get update -qq && apt-get install -y -qq build-essential cmake git
  cmake -S . -B /tmp/build -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/build -j
  cd /tmp/build && ctest --output-on-failure'
```

```
=== 빌드 ===
exit 0
경고 1건: src/event_loop.cpp:113:33: warning: enumerated and non-enumerated type
          in conditional expression [-Wextra]          ← 기존 건(06-15부터 존재)
신규 TU(StaticFileHandler.cpp) 경고 0

=== ctest ===
100% tests passed, 0 tests failed out of 34
Total Test time (real) = 2.03 sec        (integration.idle_cpu 포함)
```

2-1 해소로 링크가 통과하며 `ctest` 전체가 다시 돈다. 테스트 건수는 08-10과 동일한 34건 — 이번 작업에 대응하는 테스트를 추가하지 않았기 때문이다(4장 ⚪ 참고).

### 3-2. 신규/변경 TU 경고 검사 (gcc + clang 양쪽)

```bash
# gcc 12 (컨테이너)
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -c src/http/StaticFileHandler.cpp -o /tmp/a.o
# Apple clang (macOS 로컬)
c++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -fsyntax-only \
    src/http/StaticFileHandler.cpp src/connection.cpp
```

```
gcc   : 경고 0
clang : 경고 0    (2-4 해소 전에는 -Wdefaulted-function-deleted 1건)
```

`event_loop.cpp`/`main.cpp`는 macOS에 `sys/epoll.h`가 없어 로컬 미검사 — 컨테이너 빌드로 대체(07-22 이후 동일).

### 3-3. 경로 매트릭스 (`docker compose` 기동 컨테이너 대상)

```bash
docker compose up -d --build
for p in / /index.html /./index.html /../CLAUDE.md /../../etc/passwd //etc/passwd \
         /nonexistent.html /public ; do
  printf '%-22s -> ' "$p"
  curl -s -o /dev/null -w '%{http_code} %{size_download}B\n' --max-time 3 --path-as-is \
    "http://127.0.0.1:8080$p"
done
```

```
/                      -> 200 151B     ✅ index.html 매핑
/index.html            -> 200 151B     ✅
/./index.html          -> 200 151B     ✅ lexically_normal()로 '.' 정리
/../CLAUDE.md          -> 400 0B       ✅ 차단
/../../etc/passwd      -> 400 0B       ✅ 차단
//etc/passwd           -> 400 0B       ✅ 차단
/nonexistent.html      -> 200 0B       ⚠️ 404여야 함 (4장 🟡 1)
/public                -> 200 0B       ⚠️ 디렉터리 요청도 동일
```

`--path-as-is`가 필수다. 없으면 curl이 클라이언트 쪽에서 `..`를 정규화해 traversal 검증이 무의미해진다.

`//etc/passwd`가 차단되는 경로: `target.substr(1)`이 `/etc/passwd`가 되고, `std::filesystem`에서 `base / "/etc/passwd"`는 absolute path가 base를 **대체**해 `/etc/passwd`가 된다. prefix 검사가 이를 잡는다.

배포 산출물 확인(2-5 해소):
```
$ docker exec building-http-server-server-1 ls -la /app/public
-rw-r--r-- 1 root root 151 index.html
```

### 3-4. 바이너리 무결성 · backpressure 경로 (ASan + UBSan)

`public/`에 1MB 랜덤 바이너리와 빈 파일을 넣고 검증(저장소 오염을 피해 복사본에서 수행).

```bash
head -c 1000000 /dev/urandom > public/blob.bin
: > public/empty.txt
g++ -std=c++17 -g -fsanitize=address,undefined -Iinclude <소스 7개> -o /tmp/srv
/tmp/srv 8080 &
curl -s --max-time 20 http://127.0.0.1:8080/blob.bin -o /tmp/got.bin
cmp public/blob.bin /tmp/got.bin
```

```
수신 크기: 1000000
cmp: IDENTICAL

empty.txt    -> 200 0B
nonexistent  -> 200 0B          ⚠️ 빈 파일과 없는 파일이 구별 불가

ASan/UBSan 리포트: 0건           (2-2 해소 확인)
```

1MB를 고른 이유는 `write_buf`가 한 번의 `write()`로 나가지 않아 **06-15의 `EPOLLOUT` backpressure 경로를 반드시 태우기** 때문이다. 소형 파일만 검증하면 이 분기가 한 번도 실행되지 않는다(08-10에서 clamp 분기가 미실행이었던 것과 동종의 함정). NUL 포함 바이너리가 손실 없이 전달되고 `Content-Length`가 실제 크기와 일치함을 `cmp`로 확인.

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | `ifstream` 실패 미검사 → 404 부재 | 파일을 못 열어도 `200 / Content-Length: 0`. **빈 파일과 없는 파일이 구별되지 않고**, 디렉터리 요청도 동일. 2-5를 은폐한 장본인. `if (!file)` + `reason_phrase()`에 404 추가가 필요하며, 2-3의 임시 `400`도 이때 함께 정리 |
| 🟡 | `Content-Type` 없음 | 응답 헤더에 부재(`ct=[]`). 브라우저 MIME sniffing에 의존 중이라 CSS·이미지·바이너리를 넣는 순간 어긋남. 확장자 기반 매핑 필요 |
| 🟡 | `Connection: close` 비대칭 | `Ok` 경로엔 없고 `Error`(400) 경로(`connection.cpp:28`)에만 있음. HTTP/1.1 기본은 keep-alive인데 서버는 `close_after_write`로 닫음 |
| 🟡 | 문서 root 하드코딩 | `event_loop.h`의 `StaticFileHandler("public")` + 상대 경로. 현재는 `Dockerfile`의 `WORKDIR /app`과 맞아 동작하지만 CWD 계약이 암묵적. 통합 테스트는 `fork`+`execv`로 서버를 띄우므로(06-30) HTTP 재작성 시 2-5와 같은 문제가 재발할 자리 — 인자/환경변수 주입이 적절 |
| 🟡 | `target.substr(1)`이 `path[0] == '/'` 가정 | 파서가 path 형식을 미검증(07-12 남은 과제)이라 absolute-form(`http://host/x`)·`OPTIONS *`에서 첫 글자가 잘림 |
| 🟡 | method 미검사 | `POST /` → `200 151B`로 파일 반환. `GET`/`HEAD` 외 405 필요 |
| 🟡 | `root_(root)` 불필요한 복사 | 생성자에서 `std::move(root)` 누락 |
| ⚪ | `StaticFileHandler` 테스트 부재 | `tests/CMakeLists.txt`에 미등록이라 `ctest` 34건에 이 기능 커버리지가 0. 3-3의 경로 매트릭스를 07-13의 `TEST_P` 테이블로 옮기면 소켓·epoll 없이 macOS 로컬에서 실행 가능하고, root를 임시 디렉터리로 두면 위 CWD 의존도 함께 벗어난다 |
| ⚪ | percent-encoding 미디코딩 | 현재 `/%2e%2e/`는 리터럴로 남아 우연히 안전. 디코딩을 추가할 때 **가드보다 먼저** 디코딩해야 우회가 생기지 않는다. 심볼릭 링크(`fs::weakly_canonical`) 검사도 미대응 |

---

## 5. 핵심 교훈 요약

1. **참조 멤버는 수명 계약이다.** 참조를 걸기 전에 소유자를 정한다. 스택 재사용 탓에 우연히 동작하므로 ASan 없이는 통과해 버린다 (2-2).
2. **`compare()`는 `bool`이 아니라 3-way 정수다.** 그리고 결함은 서로를 가린다 — 검사·판정·처리가 조건식 하나에 뭉쳐 있으면 부분 수정이 traversal 개방과 정상 파일 차단 사이를 오갈 뿐이다. **정상 경로와 차단 경로를 함께 검증**해야 어느 쪽으로 기울었는지 드러난다 (2-3).
3. **오류를 오류로 보고하지 않는 코드는 남의 문제를 자기 문제로 위장시킨다.** `ifstream` 실패를 `200 / 0바이트`로 흘려보낸 탓에, 배포 설정(`Dockerfile`)의 `public/` 누락이 "서버가 이상하다"로 보였다. 404 하나면 즉시 드러났을 문제다 (2-5).
4. **빌드 통과와 배포 성공은 다른 사건이다.** 런타임 자산은 코드와 별개로 이미지에 들어가야 하고, 상대 경로를 쓰는 순간 CWD가 암묵적 계약이 된다 (2-5).
5. **컴파일러를 하나만 믿지 않는다.** gcc가 침묵한 특수 멤버 함수 진단을 clang이 잡았다. CI(gcc)와 로컬(clang)을 함께 돌리는 구성이 실제로 값을 했다 (2-4).
6. **경계 케이스는 경로를 실제로 태우는 크기로 검증한다.** 1MB 바이너리라야 `EPOLLOUT` backpressure 분기가 돌고, 텍스트가 아니라 `cmp`라야 바이너리 손실이 잡힌다 (3-4).
