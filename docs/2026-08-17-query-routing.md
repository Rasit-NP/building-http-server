# query-routing — 작업 & 트러블슈팅 로그

- **날짜**: 2026-08-17
- **작업**: 08-14 남은 과제였던 query string 미분리(`/index.html?v=1` → 404)를 해소하고, 응답 생성 분기를 `Connection`에서 떼어내 `route` 모듈로 옮기면서 `/echo` 라우트를 도입. 분리 작업 중 request line 오프셋 계산에 섞여 있던 두 관례를 절대 인덱스로 통일
- **결과**: Ubuntu 22.04 컨테이너 전체 빌드 성공(신규 TU 경고 0)·`ctest` **51/51 통과**(2.05s, 48 → 51). 커밋 2개를 각각 독립 빌드해 중간 상태도 성립함을 확인. 기동 컨테이너에서 경로·라우팅 매트릭스 9종 실측. mutation testing 4종 **2/4 검출**(미검출 2건 중 1건은 equivalent mutant, 1건은 미해소 — 4장 🟡)

---

## 1. 작업 개요

08-14 로그 4장의 "query string 미분리"는 정적 서버로서 실사용에 바로 걸리는 항목이었다. `?v=1` 같은 cache-busting 파라미터가 붙은 순간 `StaticFileHandler`가 `public/index.html?v=1`을 열려다 실패해 404가 나갔다.

같은 작업에서 `/echo`를 추가했다. `Connection::on_readable()`이 `handler_.handle(req)`를 직접 부르는 구조에서는 "정적 파일이 아닌 응답"을 만들 자리가 없었기 때문에, path에 따른 분기를 별도 TU로 떼어냈다.

| 항목 | Before | After |
|------|--------|-------|
| request target | 전체를 `path`에 담음 (`/index.html?v=1` → `404`) | `'?'` 기준으로 `path`/`query` 분리 |
| `HttpRequest` 필드 | `method`/`path`/`version` | `query` 및 `query_off`/`query_len` 추가 |
| request line 오프셋 관례 | `method`는 절대 인덱스, `path`/`version`은 `offset_` 중복 가산 | 전부 `buffer_` 기준 절대 인덱스 |
| 응답 분기 | `Connection`이 `StaticFileHandler::handle()`을 직접 호출 | `route()`가 path에 따라 분기 |
| `/echo` | 없음 | request body를 그대로 응답 body로 반환 |
| `public/` | `index.html` | `style.css`·`image.png` 추가 (`text/css`·`image/png` 경로 실주행) |
| 테스트 | 48건 | 51건 (`QueryTest` 3건) |

### 설계 — 라우팅을 `Connection`에서 떼어낸다

`Connection`은 06-14 이후 소켓 I/O와 파싱만 담당해 왔고, 응답 내용은 08-11에 `StaticFileHandler`로 위임했다. 여기에 `/echo`를 넣는 방법은 셋이었다.

1. **`Connection::on_readable()`에 `if (req.path == "/echo")` 추가** — 가장 짧지만 `Connection`이 다시 응답 내용을 알게 된다.
2. **`StaticFileHandler`에 echo 분기 추가** — 이름과 책임이 어긋난다.
3. **`route()` 자유 함수를 별도 TU로 분리** — `Connection`은 `route(req, handler_)` 한 줄만 안다.

3번을 채택했다. `route()`는 `HttpRequest`를 받아 `HttpResponse`를 돌려주는 순수 함수라 소켓 없이 테스트 가능하고, 라우트가 늘어도 `Connection`은 그대로다. `StaticFileHandler`를 `route()`의 파라미터로 받는 이유는 handler를 `EventLoop`가 값으로 소유하기 때문이다(08-11 2-2에서 dangling reference를 겪은 뒤 정착한 소유 구조).

### 설계 — query를 분리하되 해석하지는 않는다

이번 범위는 **request target을 `path`와 `query`로 자르는 것까지**다. percent-decoding과 `k=v` 분해는 넣지 않았다. `path`가 짧아져야 `StaticFileHandler`가 정상 동작하는 게 이번 목적이고, decoding은 traversal 검사보다 **먼저** 수행해야 하는 순서 제약이 있어(08-11 남은 과제) 별도 작업으로 다루는 편이 안전하다. 그 결과 `query`는 현재 파싱만 되고 읽는 쪽이 없다(4장 🟡).

`'?'`가 없는 경우 `query_off`를 `path_off`와 같게 두고 `query_len`을 0으로 둔다. `query`가 항상 유효한 포인터를 가리키게 해서, 호출자가 길이를 확인하지 않고 `string_view`를 만져도 문제가 없게 하기 위함이다.

---

## 2. 트러블슈팅

### 2-1. 🟡 `#include "http/route.cpp"` — 헤더 없이 .cpp를 직접 include

- **증상**: 빌드와 `ctest` 48/48이 모두 통과했다. 그런데 `route.cpp`를 `src/CMakeLists.txt`에 등록하는 순간 링크가 깨진다. `connection.o`와 `route.o`를 함께 링크해 재현했다:
  ```
  route.cpp:(.text+0x0):   multiple definition of `make_echo_response(HttpRequest const&)';
                           /tmp/connection.o:connection.cpp:(.text+0x0): first defined here
  route.cpp:(.text+0x174): multiple definition of `route(HttpRequest const&, StaticFileHandler const&)';
                           /tmp/connection.o:connection.cpp:(.text+0x174): first defined here
  ```
  `nm`으로 보면 `connection.o` 안에 두 심볼이 external로 들어 있다:
  ```
  0000000000000000 T make_echo_response(HttpRequest const&)
  0000000000000174 T route(HttpRequest const&, StaticFileHandler const&)
  ```
- **원인**: 선언용 헤더 없이 정의가 든 `.cpp`를 include했다. 두 함수 모두 external linkage라 `route.cpp`가 별도 TU로도 컴파일되면 정의가 둘이 된다. **빌드가 성립한 유일한 이유는 `route.cpp`가 CMake 소스 목록에 없었다는 우연**이었다.
- **부수 피해**: `tests/CMakeLists.txt`에서 `route()`를 링크할 방법이 없어 라우팅 테스트를 작성하는 것 자체가 불가능했다. 실제로 확인:
  ```
  undefined reference to `route(HttpRequest const&, StaticFileHandler const&)'
  ```
- **해결**: `include/http/route.h` 신설(두 함수 선언), `connection.cpp`가 헤더를 include, `src/CMakeLists.txt`의 `http_server` 소스 목록에 `http/route.cpp` 추가. 재현 명령을 다시 돌려 `multiple definition` 출력이 사라진 것을 확인했다.
- **교훈**: 새 TU는 **헤더 + 소스 + CMake 등록**이 한 묶음이다. 셋 중 하나만 빠져도 빌드는 통과할 수 있는데, 이번처럼 "등록하지 않았기 때문에 성립"하는 상태는 다음 사람이 등록하는 순간 깨진다. 08-11 2-1(`StaticFileHandler.cpp` 미등록으로 `undefined reference`)이 "등록을 빠뜨려 실패"였다면 이번은 같은 원인이 반대 방향으로 나타난 형태다.

### 2-2. 🟡 `'?'` 탐색 상한이 `sp2`가 아닌 `crlf` — `query_len` size_t underflow

- **증상**: `'?'`가 version 자리에 있는 요청에서 `query_len`이 wrap 된다. ASan/UBSan 프로브 실측:
  ```
  ? in version   result=Error  path_off=4 path_len=10 query_off=15 query_len=18446744073709551606
  ```
  이 길이 그대로 `parse()`의 `State::Done`에서 `string_view`가 **실제로 생성된다**.
- **원인**: `buffer_.find('?', sp1 + 1)`의 상한 검사를 `q >= crlf`로 두었다. `'?'`가 `sp2`와 `crlf` 사이(=version 필드)에 있으면 else 분기로 들어가 `query_len = sp2 - (q + 1)`이 음수가 되고, `size_t`라 `2^64`로 wrap 된다. `GET / HTTP/1.1?x`의 경우 `sp1=3`, `sp2=5`, `q=14`, `crlf=16`이므로 `path_len = 14 - 4 = 10`(→ `path`가 `"/ HTTP/1.1"`), `query_len = 5 - 15 = -10`이다.
- **해결**: 상한을 `q >= sp2`로 바꿔 `'?'` 탐색 범위를 request target 안으로 제한. 수정 후 같은 입력에서 `path_len=1`, `query_off=4`, `query_len=0`.
- **교훈**: `size_t` 두 값의 차를 쓸 때는 대소 관계가 **구조적으로** 보장되는지 확인해야 한다. 그리고 이 결함이 밖으로 새지 않았던 이유는 뒤이은 `isValidVersion()`이 `version.size() != 8`로 걸러줬기 때문인데, 이건 검증 **순서에 우연히 기댄** 안전망이다. `request_.query`를 읽는 코드가 하나라도 생기면 그대로 out-of-bounds read가 된다. 08-10의 `take = std::min(available, body_remaining_)` clamp와 같은 계열의 실수다.

### 2-3. 🟡 `offset_` 이중 가산 관례를 신규 필드가 그대로 복제

- **증상**: 현재 코드로는 무증상이다. `offset_ != 0`인 상태에서 `parseRequestLine()`을 부르는 상황(keep-alive에서 두 번째 요청)을 프로브로 재현하면 드러난다:
  ```
  [1] 첫 요청 (GET /first HTTP/1.1)        result=Ok    offset_=32
  >>> offset_=32 유지한 채 두 번째 요청(GET /second?a=1 HTTP/1.1) 파싱
  [2] 두 번째 요청                          result=Error
      method_off=32 len=3          ← 정상
      path_off=68    len=18446744073709551591   (정답 36 / 7)
      query_off=76   len=18446744073709551587   (정답 44 / 3)
      version_off=80 len=18446744073709551592   (정답 48 / 8)
  ```
- **원인**: `std::string::find`의 두 번째 인자는 탐색 시작 위치일 뿐 반환값은 언제나 절대 인덱스인데, `path_off = offset_ + sp1 + 1`처럼 `offset_`을 한 번 더 더하고 있었다. 같은 함수의 `method_off = offset_` / `method_len = sp1 - method_off`는 절대 인덱스 관례를 따르므로 **한 함수 안에 두 관례가 섞여 있었다**. `offset_ == 0`일 때만 두 식이 같은 값이라 지금까지 드러나지 않았다 — `state_`를 `RequestLine`으로 되돌리는 곳이 `reset()`뿐이고 거기서 `offset_ = 0`도 같이 하기 때문이다.
  ```
  153:    offset_ = crlf + 2;           ← parseRequestLine 끝, 직후 state_ = Headers
  164:    offset_ = line_end + 2;       ← parseHeaders
  215:    offset_ += take;              ← parseBody
  227:    state_ = State::RequestLine;  ┐ reset() — 항상 함께 실행
  229:    offset_ = 0;                  ┘
  ```
  오늘 추가한 `query_off = offset_ + q + 1`은 이 잘못된 관례를 그대로 복사한 것이었다. 잠재 결함을 가진 필드가 2개에서 3개로 늘어난 셈이다.
- **해결**: `path_off`/`query_off`/`version_off`를 각각 `sp1 + 1`, `q + 1`, `sp2 + 1`로 통일. `offset_ == 0`이므로 동작 변화는 없고 `ctest`도 그대로 통과한다. 프로브의 `[2]`는 이제 `path=[/second] query=[a=1] version=[HTTP/1.1]`로 정상 파싱된다.
- **교훈**: **한 함수에 두 관례가 섞이면 새 코드가 잘못된 쪽을 복제한다.** 오늘 실제로 그랬다. 그리고 이 수정은 어떤 테스트로도 검증할 수 없다(3-4 M2) — 관례를 통일해 두는 것 자체가 유일한 방어다. 부수 효과로 `parseRequestLine()`이 `offset_ != 0`에서도 정확해져, keep-alive 도입 시 이 함수는 손대지 않아도 된다.

### 2-4. 🔴 `routes` 라이브러리에 include directory 미지정 — 빌드 차단

- **증상**: 라우팅 테스트를 쓸 수 있도록 `tests/CMakeLists.txt`에 `add_library(routes ../src/http/route.cpp)`를 추가하고 `tests` 타깃에 링크했더니 빌드가 멈췄다.
  ```
  [  8%] Building CXX object tests/CMakeFiles/routes.dir/__/src/http/route.cpp.o
  /src/src/http/route.cpp:1:11: fatal error: http/route.h: No such file or directory
  gmake[2]: *** [tests/CMakeFiles/routes.dir/build.make:76: tests/CMakeFiles/routes.dir/__/src/http/route.cpp.o] Error 1
  gmake[1]: *** [CMakeFiles/Makefile2:432: tests/CMakeFiles/routes.dir/all] Error 2
  === ctest ===
  Errors while running CTest
  ```
- **원인**: 기존 4개 라이브러리(`sockets`/`parsers`/`responses`/`handlers`)는 각각 `target_include_directories(... PUBLIC ${CMAKE_SOURCE_DIR}/include)`를 갖는데 `routes`에만 그 줄이 없었다. `add_library` 타깃은 디렉터리 스코프 include 경로를 자동으로 물려받지 않으므로 `routes`만 `include/`를 못 봤다. `src/CMakeLists.txt`의 `http_server`는 자체 `target_include_directories`가 있어 같은 `route.cpp`를 정상 컴파일했다(로그의 `[38%] Building ... http_server.dir/http/route.cpp.o`) — **같은 파일이 한 타깃에서는 되고 다른 타깃에서는 안 되는** 형태였다. `-DCMAKE_CXX_FLAGS=-I/src/include`를 강제 주입하고 다시 빌드하니 `ctest` 51/51이 통과해 원인이 확정됐다.
- **해결**: `route()`를 테스트할 계획이 없으므로 `routes` 등록 자체를 원복했다. `tests/CMakeLists.txt`의 라이브러리는 대응 테스트 파일과 1:1이고(`sockets`↔`tests.cpp`, `parsers`↔`HttpRequestParserTest.cpp`, `responses`↔`HttpResponseTest.cpp`, `handlers`↔`StaticFileHandlerTest.cpp`), `connection.cpp`·`event_loop.cpp`·`main.cpp`가 등록되지 않은 것과 같은 자리에 `route.cpp`를 두는 편이 관례에 맞는다.
- **교훈**: CMake 타깃 추가는 **소스 등록 + include directory 지정**이 한 쌍이다. 08-11 2-1과 동종 재발이며, 이번에는 "등록은 했으나 절반만" 형태로 나타났다. 그리고 테스트를 쓸 계획이 없다면 라이브러리 등록 자체가 불필요하다 — 링크 가능성을 미리 열어두는 것은 이 저장소의 관례가 아니다.

---

## 3. 빌드 · 검증

호스트(macOS/arm64)는 `sys/epoll.h`가 없어 `event_loop.cpp`·`main.cpp`가 컴파일되지 않는다(06-09부터의 기존 제약). 검증은 전부 Linux 컨테이너에서 수행했다.

### 3-1. 전체 빌드 · `ctest` (Ubuntu 22.04 컨테이너)

```bash
docker build --target builder -t hs-builder .   # ubuntu:22.04 + build-essential cmake git

docker run --rm -v "$PWD":/src -w /src hs-builder sh -c '
  rm -rf /tmp/build
  cmake -S . -B /tmp/build -DCMAKE_BUILD_TYPE=Debug
  cmake --build /tmp/build -j
  cd /tmp/build && ctest'
```

```
=== gcc ===
g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0

=== 빌드 ===
error 0건
warning 1건: src/event_loop.cpp:113:33: warning: enumerated and non-enumerated type
             in conditional expression [-Wextra]        ← 기존 건(06-15부터 존재)
신규 TU(http/route.cpp): 경고 0

=== ctest ===
100% tests passed, 0 tests failed out of 51
Total Test time (real) =   2.05 sec
```

08-14의 48건에서 3건 증가(`QueryTest` 3케이스). 2-4의 원복 전에는 이 명령이 `Errors while running CTest`로 끝났다.

### 3-2. 커밋별 독립 빌드

코드 커밋(`feat`)과 테스트 커밋(`test`) 사이에서 빌드가 깨지지 않는지 확인했다. `HttpRequestParserTest.cpp`가 `http/route.h`를 include하므로 `route.h`를 만드는 커밋이 테스트 커밋보다 **앞서야** 한다.

```bash
for c in 5a5288c 1e121f3; do
  mkdir -p wt/$c && git archive $c | tar -x -C wt/$c
done
docker run --rm -v "$PWD/wt":/wt hs-builder sh -c '
  for c in 5a5288c 1e121f3; do
    cd /wt/$c && cmake -S . -B b && cmake --build b -j && (cd b && ctest)
  done'
```

```
5a5288c  feat: query string 분리 및 /echo 라우팅 도입
         BUILD OK   100% tests passed, 0 tests failed out of 48
1e121f3  test: query string 파싱 테스트 및 케이스 구조체 공통화
         BUILD OK   100% tests passed, 0 tests failed out of 51
```

### 3-3. 파서 프로브 (ASan/UBSan)

`gtest` 밖에서 `path`/`query`의 오프셋·길이를 직접 관찰했다. 2-2·2-3의 근거다.

```bash
g++ -std=c++17 -g -fsanitize=address,undefined -Iinclude \
    probe.cpp src/http/HttpRequestParser.cpp -o probe && ./probe
```

```
normal query       result=Ok     path=[/echo] query=[a=1]
no query           result=Ok     path=[/echo] query=[]          query_off=path_off, query_len=0
root + query       result=Ok     path=[/]     query=[a=1&b=2]
empty query        result=Ok     path=[/echo] query=[]          "/echo?" 
? in version       result=Error  path_len=1 query_off=4 query_len=0
? in header only   result=Ok     path=[/echo] query=[]          "X: a?b" 는 query 로 오인하지 않음
split request line result=Ok     path=[/echo] query=[a=1]       1바이트씩 분할 수신
```

ASan/UBSan 리포트 0. `? in version` 행은 수정 전 `query_len=18446744073709551606`이었다(2-2).

### 3-4. mutation testing — 2/4 검출

"통과한다"와 "검증한다"는 다르므로, 이번에 고친 지점을 하나씩 되돌려 `QueryTest`가 잡아내는지 확인했다. 저장소를 복사해 변이시키고 `ctest`를 돌렸다.

| # | 변이 | 결과 |
|---|------|------|
| M1 | `q >= sp2` → `q >= crlf` (2-2 원복) | **미검출** 51/51 통과 |
| M2 | `path_off`/`query_off`/`version_off`에 `offset_ +` 재삽입 (2-3 원복) | **미검출** 51/51 통과 |
| M3 | `query_len`을 항상 `0` | 검출 (2건 실패) |
| M4 | `path_len = q - path_off` → `sp2 - path_off` (query를 path에 포함) | 검출 (2건 실패) |

**M3·M4 검출**은 query 분리의 정상 동작이 실제로 보호된다는 뜻이다.

**M1은 `Result` 기반 테스트로는 원리적으로 검출할 수 없다.** M1이 동작을 바꾸는 입력은 `'?'`가 version 자리에 있는 경우뿐인데, version은 `HTTP/d.d` 8바이트여야 하므로 `'?'`가 들어가면 `isValidVersion()`이 무조건 실패한다. 즉 수정 전후 모두 `Error`를 반환하고, 차이는 `path_len`/`query_len` 값에만 남는다. `ErrorCase`를 추가해도 통과한다. 검출하려면 오류 입력에 대해 필드를 직접 단언하거나(`EXPECT_EQ(0u, parser.request().query_len)`) 탐색 범위를 구조적으로 못박아야 한다(4장 🟡).

**M2는 equivalent mutant다.** `parseRequestLine()`은 `offset_ == 0`에서만 호출되므로 `+ offset_`은 관측 불가능하다. keep-alive를 붙이는 시점에야 관측 가능해진다. 08-10 2-5(`reset()`의 `body_remaining_` 초기화)와 같은 성격이다.

### 3-5. 경로 · 라우팅 매트릭스 (기동 컨테이너 실측)

```bash
docker run -d -p 18080:8080 -v "$PWD":/src -w /src hs-builder sh -c '
  cmake -S . -B /tmp/build && cmake --build /tmp/build -j
  mkdir -p /tmp/run && cp /tmp/build/src/http_server /tmp/run/ && cp -r /src/public /tmp/run/public
  cd /tmp/run && ./http_server'

curl -s -o /dev/null --path-as-is -w '%{http_code} %{content_type} %{size_download}B\n' ...
printf 'GET / HTTP/1.1?x\r\nHost: x\r\n\r\n' | nc -w 2 localhost 18080 | head -1
```

```
GET /                     -> 200 text/html  249B     ✅ index.html 매핑
GET /index.html?v=1       -> 200 text/html  249B     ✅ 08-14에는 404 (이번 작업의 목적)
GET /style.css?a=1&b=2    -> 200 text/css    53B     ✅ 다중 파라미터
GET /image.png            -> 200 image/png 224566B   ✅ 바이너리 + EPOLLOUT backpressure 경로
GET /nope.html            -> 404 text/html   48B     ✅
POST /echo (11B body)     -> 200 text/plain  11B     ✅ body 왕복 일치, 요청 Content-Type 전달
GET  /echo                -> 200 application/octet-stream 0B   ⚠️ method 미검사 (4장 🟡)
GET /../CLAUDE.md  (raw)  -> 404                     ✅ traversal 차단 유지
GET / HTTP/1.1?x   (raw)  -> 400 Bad Request         ✅ 서버 생존 확인 (2-2)
```

`GET / HTTP/1.1?x` 직후 `GET /`가 `200`을 돌려주는 것으로 프로세스가 살아 있음을 확인했다. 서버 로그에는 `parse error on fd=7` 한 줄만 남는다.

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | 2-2 가드가 무방비 | 3-4 M1. `q >= sp2`를 되돌려도 51/51이 통과한다. 오류 입력에 대해 `query_len`을 직접 단언하거나, `'?'` 탐색을 `[sp1+1, sp2)`로 못박아 구조적으로 성립 불가능하게 만들어야 한다 |
| 🟡 | `QueryTest`가 `Result` 미단언 | `HttpRequestParserTest.cpp:227`의 `r`에 두 번 대입하고 한 번도 쓰지 않는다. 바로 위 `BodyTest`는 `EXPECT_EQ(r, c.expected_result)`를 하는데 `QueryCase`에 `expected_result` 필드가 없어 그 줄이 빠졌다 |
| 🟡 | `tests` 타깃에 경고 플래그 없음 | 위 항목이 빌드에서 안 보이는 이유. `-Wall -Wextra -Wpedantic`은 `src/CMakeLists.txt`의 `http_server`에만 붙어 있다. 별도 컴파일하면 `warning: variable 'r' set but not used [-Wunused-but-set-variable]`가 나온다. `target_compile_options(tests PRIVATE -Wall -Wextra)` 한 줄이면 테스트 코드도 같은 기준으로 검사된다 |
| 🟡 | 테스트 파일의 dead code | `HttpRequestParserTest.cpp:4`의 `# include "http/route.h"`와 빈 `struct EchoCase: BaseCase {}`가 미사용. `routes` 라이브러리가 없어 `route()`를 호출하면 링크되지 않는다(2-4). echo 테스트를 쓸 계획이면 등록이 먼저고, 아니면 두 줄을 지운다 |
| 🟡 | `route()` 커버리지 0건 | 2-4 참조. echo body 왕복, `Content-Type` 전달·기본값, `/echo` 아닌 경로의 handler 위임이 모두 미검증 |
| 🟡 | `/echo`가 method를 검사하지 않음 | 3-5 실측. `GET /echo`가 빈 body에 `application/octet-stream`을 붙여 `200`을 낸다. echo는 POST 의미론이므로 405가 적절. 08-11에서 이월된 "method 미검사"와 함께 처리 |
| 🟡 | `/echo` 매칭이 정확 일치 | `/echo/`는 `StaticFileHandler`로 흘러 404가 된다. prefix 매칭 여부를 정할 시점 |
| 🟡 | `make_echo_response()` external linkage | `route()`만 쓰는 내부 헬퍼인데 `route.h`에 선언돼 있다. 익명 namespace가 적절하나 그러면 단위 테스트에서 직접 부를 수 없다 — 테스트 방침을 정한 뒤 결정. 08-02·08-10·08-14에 이은 네 번째 동종 항목 |
| 🟡 | `query`를 읽는 쪽이 없음 | 파싱만 되고 `route()`도 `StaticFileHandler`도 쓰지 않는다. 1장 설계 참조 |
| ⚪ | `query_cases` 커버리지 | 빈 query(`/echo?`), 다중 파라미터(`?a=1&b=2`), 헤더에만 `'?'`가 있는 경우가 빠져 있다. 셋 다 3-3 프로브에서는 확인했지만 회귀 테스트에는 없다 |
| ⚪ | percent-decoding 없음 | `?a=%20b`가 원문 그대로 남는다. 도입 시 **디코딩을 traversal 검사보다 먼저** 해야 한다(08-11에서 이월) |
| ⚪ | integration 시나리오 부활 가능 | `tests/integration/CMakeLists.txt:10-17`에 "echo 계약이 성립하지 않아 보류"로 주석 처리된 4종(`single_echo`/`concurrent_echo`/`large_payload`/`signal_shutdown`)이 있다. `POST /echo`가 생겨 HTTP 계약으로 재작성할 조건이 갖춰졌다 |
| ⚪ | include 경로 표기 혼재 | `route.h`는 `#include "http/HttpResponse.h"`, `StaticFileHandler.h`는 `#include "HttpRequest.h"`. 둘 다 동작하지만 기준이 다르다 |
| ⚪ | `index.html`의 상대 경로 `img` | `<img src="image.png">`가 root(`/`)에서만 정상. 하위 경로 페이지가 생기면 깨진다. `link`는 `/style.css`로 절대 경로를 쓰고 있어 표기도 엇갈린다 |

08-14 남은 과제 중 이번에 해소된 것은 "query string 미분리" 하나이고, HEAD 응답 body 전송·`Date` 중복 방지 부재·`gmtime_r` 반환값 미확인·`unique_name()` PID 미포함(`ctest -j` 사용 불가)·`Connection: close` 비대칭 등은 그대로 이월된다.

---

## 5. 핵심 교훈 요약

1. **"등록하지 않았기 때문에 성립하는" 빌드는 성립한 게 아니다.** `#include "*.cpp"`는 그 파일이 CMake 소스 목록에 없을 때만 링크된다. 새 TU는 헤더·소스·CMake 등록이 한 묶음이고, 셋 중 하나가 빠진 상태도 초록불일 수 있다 (2-1).
2. **`size_t` 두 값의 차는 대소 관계가 구조적으로 보장될 때만 안전하다.** 그리고 잘못된 값이 밖으로 새지 않은 이유가 "뒤이은 검증이 먼저 실패해서"라면, 그건 방어가 아니라 검증 순서에 기댄 우연이다 (2-2).
3. **한 함수에 두 관례가 섞이면 새 코드가 잘못된 쪽을 복제한다.** 오늘 추가한 `query_off`가 기존 `path_off`의 잘못된 표기를 그대로 따라가, 잠재 결함 필드가 2개에서 3개로 늘었다. 관례 통일은 동작이 안 바뀔 때 해두는 게 가장 싸다 (2-3).
4. **equivalent mutant는 테스트로 막을 수 없다.** `offset_` 관련 수정은 `offset_ == 0`이라는 현재 호출 구조 때문에 어떤 테스트로도 관측되지 않는다. 08-10에 이어 두 번째다 — 이런 항목은 테스트 대신 구조와 기록으로 방어한다 (3-4 M2).
5. **`Result`만 단언하는 테스트는 필드 계산 결함을 못 잡는다.** M1은 수정 전후 모두 `Error`를 반환하므로, 반환값 기반 케이스를 아무리 늘려도 검출되지 않는다. 무엇을 관측하는 테스트인지가 케이스 수보다 중요하다 (3-4 M1).
6. **CMake 타깃 추가는 소스 등록과 include directory 지정이 한 쌍이다.** 같은 `route.cpp`가 `http_server`에서는 컴파일되고 `routes`에서는 실패했다. 08-11 2-1의 동종 재발이며, 이번엔 "절반만 등록" 형태였다 (2-4).
7. **테스트 타깃에 경고 플래그가 없으면 테스트 코드의 결함은 보이지 않는다.** 프로덕션 소스에만 `-Wall -Wextra`를 걸어두면 테스트는 검사 대상 밖에 놓인다 (4장 🟡).
