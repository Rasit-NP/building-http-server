# request-body-parsing — 작업 & 트러블슈팅 로그

- **날짜**: 2026-08-10
- **작업**: `HttpRequestParser`에 `Content-Length` 기반 request body 파싱(`State::Body`/`parseBody()`)을 추가하고, 상충하는 중복 `Content-Length` 헤더를 거부하도록 검증 강화
- **결과**: Ubuntu 22.04 컨테이너에서 `http_server` 포함 전체 빌드 성공·`ctest` 34/34 통과. macOS 로컬 단위 테스트 33/33 통과, ASan/UBSan 리포트 0

---

## 1. 작업 개요

07-06 incremental parsing과 07-12 parser hardening으로 request line·headers까지 처리하던 `HttpRequestParser`에 body 단계를 붙였다. `Content-Length` 값을 검증해 길이를 확정하고, 그만큼을 `HttpRequest::body`에 누적한다. 작업 중 리뷰에서 message framing 모호성(중복 `Content-Length`)이 드러나 함께 처리했다.

| 항목 | Before | After |
|------|--------|-------|
| `State` | `RequestLine` / `Headers` / `Done` / `Error` | `Body` 추가 (`Headers` → `Body` → `Done`) |
| body 저장 | 없음 | `HttpRequest::body` (`std::string`) |
| body 길이 | — | `parse_content_length()`가 `std::from_chars`로 검증 후 `body_remaining_`에 설정 |
| 헤더 중복 | `headers[to_lower(name)] = trim_ows(value)` — last-wins 덮어쓰기 | 최초 값 유지, `content-length`가 상충하면 `Error` |
| `reset()` | `state_`/`buffer_`/`offset_`/`request_` | `body_remaining_ = 0` 추가 |
| 파서 테스트 | 21건 (request line·headers·error) | 33건 (`BodyTest` 10건 신설) |

### 설계 — body 길이 확정 지점

`parseHeaders()`가 빈 줄(헤더 종료 CRLF)을 만난 시점에서 `content-length`를 조회한다. 헤더가 없으면 즉시 `Done`, 값이 불량이면 `Error`, `0`이면 `Done`, 그 외에는 `State::Body`로 넘어가 `parseBody()`를 바로 호출한다. `state_ == State::Body`인 동안 `body_remaining_ >= 1`이 불변식으로 유지된다.

### 설계 — `Content-Length` 값 검증

`std::from_chars`를 쓰되 두 가지를 추가로 막는다. `ptr != end`로 `5, 5` 같은 뒷꼬리를 거부하고, `ec != std::errc()`로 20자리 overflow를 거부한다. 부호는 앞단에서 걸러 `-1`/`+1`을 배제한다.

### 설계 — body 초과분 clamp

`take = std::min(available, body_remaining_)`. 선언한 길이보다 많은 바이트가 한 번에 도착해도 body는 `Content-Length`에서 끊긴다. 이 clamp가 없으면 `body_remaining_ -= take`가 `size_t` underflow를 일으켜 파서가 `Incomplete`에 갇힌다 (2-1 참고).

---

## 2. 트러블슈팅

### 2-1. 🟡 청크 1 고정 급여로 body clamp 분기가 실행되지 않음

- **증상**: `BodyTest` 5건이 모두 통과하는데, `parseBody()`의 `take = std::min(available, body_remaining_)`을 `take = available`로 바꾼 파서에 같은 테스트를 돌려도 **20/20 그대로 통과**. clamp 제거가 검출되지 않음.
- **원인**: `feedInChunks(parser, c.input, 1)`로 청크 크기를 1로 고정한 탓. `state_ == State::Body`인 동안 `body_remaining_ >= 1`이 보장되고, 청크 1이면 `available <= 1`이므로 `available > body_remaining_`이 **성립할 수 없다**. `std::min`의 두 번째 인자가 선택되는 경로가 원리적으로 실행되지 않는다. 계측 결과 전체 테스트에서 `parseBody()` 17회 호출 중 clamp 발동 **0회**.
- **해결**: `BodyTest`가 일괄 `parse(input.data(), input.size())`와 청크 1 급여를 **모두** 수행하도록 변경. 실제 `Connection::on_readable()`이 `char buf[4096]`으로 읽으므로 일괄 급여가 운영 경로와 같다.
- **교훈**: 파서 동작은 "어떤 바이트가 들어왔는가"뿐 아니라 "**한 호출에 몇 바이트가 실려 왔는가**"에도 좌우된다. 청크 1은 증분 누적을 괴롭히는 데는 최적이지만 "부족하게 도착"을 미는 방향이라, "넘치게 도착"을 막는 가드에는 원리적으로 닿지 못한다.

### 2-2. 🟡 일괄 급여를 추가해도 clamp가 여전히 미검증

- **증상**: 2-1의 해결로 일괄 급여를 넣었는데도 clamp 제거 변이가 계속 생존. 테스트는 28/28 통과.
- **원인**: 급여 방식은 고쳤지만 **케이스 데이터**에 초과분이 없었다. 기존 5건은 `CL:10`+정확히 10바이트(`available == body_remaining_`), `CL:0`·헤더 없음(`Body` 미진입), `CL:abc`(`Body` 전 `Error`), `CL:20`+3바이트(`available < body_remaining_`)뿐. clamp는 `available > body_remaining_`일 때만 binding하므로 **선언보다 많은 바이트가 도착하는 입력**이 있어야 한다.
- **해결**: `{"POST /over HTTP/1.1\r\ncontent-length: 5\r\n\r\nhelloEXTRA", "hello", Ok}` 추가. 이후 clamp 제거 변이가 `Incomplete`/`body=helloEXTRA`로 실패하며 검출됨.
- **교훈**: 실행 경로를 여는 것(급여 방식)과 분기 조건을 만족시키는 것(입력 데이터)은 별개다. 둘 다 갖춰야 분기가 검증된다.

### 2-3. 🔴 중복 `Content-Length` 거부 로직이 목표를 못 잡고 정상 요청을 거부

- **증상**: 중복 거부를 도입했는데 실제 동작이 정반대. 막아야 할 요청은 통과하고, 통과해야 할 요청이 400.
  ```
  Content-Length: 5 / Content-Length: 3  -> Ok  body=[hel]   (Error여야 함)
  Content-Length: 3 / Content-Length: 5  -> Ok  body=[hello] (Error여야 함)
  content-length: 5 / content-length: 5  -> Error            (Ok여야 함)
  cookie: a=1 / cookie: b=2              -> Error            (Ok여야 함)
  ```
- **원인**: 세 가지가 겹침.
  1. `request_.headers.find(name)`의 `name`이 **정규화 전 원문**인데 저장 키는 `to_lower(name)`. 대문자를 포함한 헤더는 `find`가 항상 miss → 첫 분기로 빠져 덮어쓰고 **중복 검사 자체가 실행되지 않음**. `Content-Length`가 정확히 이 경우다.
  2. 비교 대상이 `trim_ows` 이전의 `value`인데 저장된 값은 `trim_ows(value)`. 콜론 뒤 OWS가 있으면 `" 5" != "5"`로 **항상** 불일치 → 동일 값 중복도 `Error`.
  3. 거부 범위가 전 헤더. `Cookie`/`Via`/`X-Forwarded-For`처럼 반복이 정상인 헤더도 400. 원인 1 때문에 대문자 표기가 우연히 빠져나가 피해가 가려져 있었고, 원인 1만 고치면 오히려 전면화되는 관계였다.
- **해결**: `name`/`value`를 **선언 시점에** `http::to_lower`/`http::trim_ows`로 정규화해 `find`·저장·비교가 모두 같은 형태를 쓰게 하고, 거부를 `name == "content-length"`로 한정. 수정 후 중복 헤더 12개 조합 전수 확인에서 불일치 0건.
- **교훈**: 정규화한 값과 정규화 전 값이 한 블록에 공존하면 어느 쪽을 쓰는지 반드시 틀린다. 정규화는 값이 만들어지는 지점에서 한 번만 수행하고, 이후로는 정규화된 변수만 노출한다. 또한 서로를 가리는 결함이 겹쳐 있을 때는 하나만 고치면 상태가 악화될 수 있으므로 함께 본다.

### 2-4. 🟡 중복 검출 테스트가 혼합 표기만 담아 직전 버그를 못 잡음

- **증상**: 2-3 해결 후 중복 케이스 2건을 추가했는데, 원인 1(`find`에 원문 사용)을 되돌린 파서에서 **31/31 그대로 통과**. 방금 겪은 버그의 재발을 테스트가 막지 못함.
- **원인**: 추가한 두 케이스가 모두 `Content-Length` + `content-length` **혼합 표기**였다. 둘째 줄의 원문이 이미 소문자라, 첫 줄이 소문자 키로 저장해둔 것과 글자까지 일치해 버그 코드에서도 `find`가 히트한다. 버그는 **양쪽이 같은 대문자 표기**일 때만 드러나며, 그것이 실제 클라이언트가 보내는 형태다.
  ```
  버그 파서에서:
    Content-Length:1 / content-length:2 (혼합)   -> Error   테스트에 있음, 통과해버림
    Content-Length:1 / Content-Length:2 (동일)   -> Ok      테스트에 없음, 이것이 버그
  ```
- **해결**: 동일 대문자 표기 케이스(`/dupCL3`)와 반복 일반 헤더 케이스(`/dupCookie`) 2건 추가. 이후 원인 1·원인 3 변이가 각각 검출됨.
- **교훈**: 정규화 로직의 테스트는 **정규화 전후가 실제로 달라지는 입력**을 써야 한다. 우연히 정규화가 필요 없는 입력은 정규화 버그를 그대로 통과시킨다.

### 2-5. ⚪ `reset()`의 `body_remaining_` 초기화는 테스트로 관측 불가

- **증상**: `reset()`에 `body_remaining_ = 0`을 추가하고 테스트에서 `reset()`을 호출하도록 했는데도, 해당 줄을 제거한 변이가 계속 생존.
- **원인**: `parseBody()`에 도달하는 유일한 경로가 `parseHeaders()`의 `body_remaining_ = len` 대입을 반드시 통과한다. stale 값은 읽히기 전에 항상 덮어써지므로 public API로 차이를 관측할 수 없다(equivalent mutant). partial body 후 `reset()` → CL 없음/`CL:0`/`CL:3`/`CL:200` 부분/에러 요청/헤더 불완전 6개 시나리오에서 정상 파서와 변이 파서의 출력이 `diff` 완전 일치함을 확인.
- **해결**: 코드는 방어적으로 유지하고, 테스트 추가는 하지 않음. 향후 `State::Body` 진입 경로가 늘면(예: chunked) 관측 가능해진다.
- **교훈**: mutation testing에서 생존한 변이가 전부 테스트 결함은 아니다. 관측 불가능한 변이를 먼저 가려내야 잡을 수 없는 것을 쫓느라 테스트를 늘리는 낭비를 피한다.

---

## 3. 빌드 · 검증

### 3-1. macOS 로컬 — 단위 테스트

```bash
cmake --build build --target tests
./build/tests/tests
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -c src/http/HttpRequestParser.cpp -o /dev/null
```

```
[==========] 33 tests from 7 test suites ran.
[  PASSED  ] 33 tests.

파서 TU: exit 0, 경고 0
```

`BodyTest` 10건이 각 케이스마다 일괄 급여와 청크 1 급여를 모두 수행한다(2-1 해소).

### 3-2. macOS 로컬 — sanitizer

```bash
clang++ -std=c++17 -g -fsanitize=address,undefined -Iinclude \
    tests/HttpRequestParserTest.cpp src/http/HttpRequestParser.cpp \
    build/lib/libgtest.a build/lib/libgtest_main.a -o san
./san
```

```
[==========] 25 tests from 5 test suites ran.
[  PASSED  ] 25 tests.
ASan/UBSan 리포트 0건
```

### 3-3. macOS 로컬 — `ctest`

```bash
ctest --test-dir build --output-on-failure
```

```
97% tests passed, 1 tests failed out of 34
34 - integration.idle_cpu (Timeout 30.03 sec)
```

`integration.idle_cpu`는 epoll 기반 서버를 실제 구동하므로 macOS에서는 `sys/epoll.h` 부재로 실행 불가. 08-02와 동일 상황이며 3-4에서 Linux로 해소.

### 3-4. Linux — Ubuntu 22.04 컨테이너 전체 빌드·`ctest`

epoll 의존 TU(`main.cpp`/`event_loop.cpp`/`connection.cpp`)를 포함한 전체 링크를 확인.

```bash
docker build --target builder -t hs-verify .
docker run --rm hs-verify sh -c "ls -la /src/build/src/http_server && cd /src && ctest --test-dir build --output-on-failure"
```

```
[100%] Built target tests
-rwxr-xr-x 1 root root 47776 Aug 10 01:38 /src/build/src/http_server

100% tests passed, 0 tests failed out of 34
Total Test time (real) = 2.03 sec
```

`http_server` 실행 파일 생성, `integration.idle_cpu` 포함 **34/34 통과**. 컴파일 경고는 `event_loop.cpp:114`의 `[-Wextra]` 1건뿐이며 07-22에서 확인된 기존 건으로, 이번 작업에서 추가된 경고는 0건.

### 3-5. Mutation testing — 테스트가 결함을 검출하는지 확인

파서에 결함을 주입한 사본을 만들고 **테스트를 무수정으로** 돌려, 실패하면 그 결함이 방어된다고 판단했다.

| 변이 | 결과 |
|------|------|
| M1 body clamp 제거(`take = available`) | KILLED |
| M2 짧은 body를 `Ok`로 조기 완료 | KILLED |
| M3 `parse_content_length` 검증 무력화 | KILLED |
| M4 `State::Body` 진입 생략 | KILLED |
| M5 `reset()`의 `body_remaining_` 초기화 제거 | SURVIVED (equivalent, 2-5) |
| M6 `append(buffer_, 0, take)` — offset 무시 | KILLED |
| D1 `find()`에 정규화 전 원문 사용 | KILLED |
| D2 비교를 `trim_ows` 이전 값으로 | KILLED |
| D3 `content-length` 한정 가드 제거 | KILLED |
| D4 중복 검출 제거(last-wins 복귀) | KILLED |
| D5 충돌 조건 반전(`==`일 때 거부) | KILLED |
| D6 헤더명 `to_lower` 정규화 제거 | KILLED |

12건 중 11건 검출, 생존 1건은 관측 불가능한 equivalent mutant. 2-1·2-2·2-4는 모두 이 방법으로 발견됐다.

### 3-6. 동작 확인 — 중복 헤더 12개 조합

```
[A] 상충 중복 CL (대문자/소문자/혼합 4종)          -> 전부 Error
[B] 동일 값 중복 CL (대문자/소문자 2종)            -> 전부 Ok
[C] 반복 일반 헤더 Cookie/cookie/Via/x-f-f 4종     -> 전부 Ok
[D] 단일 헤더 Host/host 2종                        -> 전부 Ok
불일치 0건
```

OWS 변형(공백 2개, 탭, 뒤쪽 OWS), 3회 이상 중복(`5/5/3`, `5/3/5`), 불량값 은폐 케이스(`CL: abc` → `CL: 5`)도 함께 확인.

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | `Transfer-Encoding` 미대응 | `TE: chunked` + `CL: 5` 요청이 chunk framing 바이트(`5\r\nhe`)를 body로 삼고 10바이트를 버퍼에 남김. RFC 9112 §6.3은 `TE` 존재 시 `Content-Length`를 무시하고 chunked로 처리하거나 거부하도록 요구. 중복 CL을 막은 지금 남은 유일한 framing 모호성 |
| 🟡 | body에도 8KB 상한 적용 | `kMaxBufferBytes`가 헤더·body 공용이라 9000바이트 body가 `Error` → 400. 의미상 413이며 헤더 상한과 body 상한 분리 필요. 07-22의 `integration.large_payload`(4MB)와 같은 뿌리 |
| 🟡 | `parse_content_length` external linkage | `nm`상 `T` 심볼. 08-02 2-1(`reason_phrase()`)과 동종 재발이며 기존 `find_crlf`도 동일. 익명 namespace/`static`으로 한정 필요 |
| ⚪ | 반복 일반 헤더 값 유실 | 최초 값만 남고 이후가 버려짐(`Cookie: a=1` / `b=2` → `a=1`). RFC 9110 §5.3은 쉼표 결합 권고 |
| ⚪ | `Connection`이 `req.body` 미사용 | 파싱만 되어 있고 응답 생성에 반영되지 않음 |
| ⚪ | keep-alive (`reset()` 호출) | 07-22부터 이월. 잔여 바이트 이월 정책과 함께 설계 필요 |

---

## 5. 핵심 교훈 요약

1. **정규화는 값이 만들어지는 지점에서 한 번만 한다.** 정규화 전후 값이 한 블록에 공존하면 `find`는 원문으로, 저장은 소문자로 하는 식의 어긋남이 생긴다. 게다가 이런 결함은 서로를 가려서(대문자 miss가 과잉 거부를 은폐), 하나만 고치면 오히려 악화될 수 있다 (2-3).
2. **테스트 통과는 "코드가 맞다"가 아니라 "이 테스트로는 틀린 걸 못 찾았다"이다.** 확실히 틀린 코드를 주입해 테스트가 실패하는지 봐야 구분된다. 이번 작업의 세 건(2-1·2-2·2-4)은 전부 초록불 상태에서 이 방법으로만 드러났다.
3. **파서 테스트는 급여 방식도 입력이다.** 어떤 바이트가 들어왔는가뿐 아니라 한 호출에 몇 바이트가 실려 왔는가가 분기를 가른다. 청크 1 고정은 증분 누적에는 강하지만 "넘치게 도착"을 막는 가드에는 원리적으로 닿지 못한다 (2-1).
4. **정규화 로직의 테스트는 정규화가 실제로 필요한 입력을 써야 한다.** 우연히 정규화 전후가 같은 입력(이미 소문자인 헤더명)은 정규화 버그를 그대로 통과시킨다 (2-4).
5. **생존한 변이가 전부 테스트 결함은 아니다.** 관측 불가능한 equivalent mutant를 먼저 가려내지 않으면, 잡을 수 없는 것을 쫓느라 테스트를 늘리게 된다 (2-5).
