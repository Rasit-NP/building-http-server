# httpresponse-serialization — 작업 & 트러블슈팅 로그

- **날짜**: 2026-08-02
- **작업**: `connection.cpp`에 인라인돼 있던 `build_response()`를 `HttpResponse` 클래스 + `serialize()`로 분리하고, 단위 테스트 추가
- **결과**: macOS 로컬(Apple clang) 단위 테스트 23/23 통과. `HttpResponse.cpp` 단독 컴파일·`connection.cpp` 구문 검사 통과. epoll 의존 TU(`main.cpp`/`event_loop.cpp`)는 `sys/epoll.h` 부재로 미빌드 → CI 대기

---

## 1. 작업 개요

07-22에 도입한 파일 스코프 `build_response()`(status line + `Content-Length` + `Connection: close`를 손으로 조립)를 걷어내고, 응답을 값 타입 `HttpResponse`로 표현한 뒤 `serialize()`가 HTTP/1.1 응답 문자열을 생성하도록 분리했다. 07-22 남은 과제 중 "`HttpResponse` 타입 도입"을 처리한 것이다.

| 항목 | Before | After |
|------|--------|-------|
| 응답 조립 | `connection.cpp`의 익명 namespace `build_response(status_line, body)` | `HttpResponse` 클래스(`status_code`/`headers`/`body`) + `serialize()` |
| status line | `"200 OK"` 등 문자열을 호출부가 직접 전달 | `status_code`(int)로부터 `reason_phrase()`가 매핑 |
| `Content-Length` | `build_response` 내부에서 `body.size()`로 계산 | `serialize()`가 `body.size()`로 자동 산출, `headers` 중복 지정은 skip |
| 호출부(`on_readable`) | `write_buf.append(build_response(...))` | `HttpResponse res;` 설정 후 `write_buf.append(res.serialize())` |
| 빌드 대상 | — | `src/CMakeLists.txt`에 `http/HttpResponse.cpp` 추가 |
| 단위 테스트 | 없음 | `HttpResponseTest`(value-parameterized) + `responses` 라이브러리 등록 |

### 설계 원칙 — `Content-Length`는 `serialize()`의 단일 책임

`Content-Length`를 호출부가 헤더로 넣게 두면 body와 값이 어긋날 수 있다. `serialize()`가 `body.size()`로 **항상 재산출**하고, `headers`에 `Content-Length`가 들어와도 루프에서 skip해 중복 emit을 막는다. 호출부는 값을 신경 쓰지 않는다.

---

## 2. 트러블슈팅

### 2-1. 🟡 `reason_phrase()`가 external linkage

- **증상**: `HttpResponse.cpp`의 `reason_phrase(int)`가 파일 스코프에 그대로 정의되어 external linkage를 가짐. 다른 TU가 같은 이름 함수를 정의하면 링커 충돌(ODR 위반) 소지.
- **원인**: `build_response()`를 옮기면서, 원본이 익명 namespace 안에 있던 사실(내부 링키지 위생)이 함께 옮겨지지 않음.
- **해결**: `reason_phrase()`를 `namespace { ... }`로 감싸 내부 링키지로 한정.
- **교훈**: 번역 단위 내부에서만 쓰는 helper는 익명 namespace/`static`으로 링키지를 좁힌다. 이 위생을 빠뜨리면 지금은 무해해도 파일이 늘면 충돌한다 (2-3에서 실제로 발생).

### 2-2. 🟡 `Content-Length`를 호출부가 수동 지정 — footgun

- **증상**: 초기 `serialize()`는 `Content-Length`를 만들지 않아, `connection.cpp`의 `Ok`/`Error` 두 분기가 각각 `res.headers.emplace_back("Content-Length", std::to_string(res.body.size()))`를 직접 넣음. body와 값이 어긋날 여지가 있고, 두 분기에 중복.
- **원인**: `build_response()`가 내부에서 계산하던 책임이 분리 과정에서 호출부로 새어 나감.
- **해결**: `serialize()`가 헤더 루프 뒤에 `Content-Length: <body.size()>`를 자동 emit. 루프에서 `if (name == "Content-Length") continue;`로 호출부가 넣은 중복은 무시. 호출부의 수동 지정 두 줄 제거.
- **교훈**: body에서 파생되는 헤더는 직렬화 지점에서 단일 책임으로 산출한다. 자동 emit 도입 시, 호출부가 같은 헤더를 넣을 가능성을 skip 가드로 함께 막아야 이중 출력이 안 난다.

### 2-3. 🔴 단위 테스트 전역 `cases` 중복 심볼 — 링크 실패

- **증상**: `tests` 실행 파일 링크 실패.
  ```
  duplicate symbol '_cases' in:
      .../HttpResponseTest.cpp.o
      .../HttpRequestParserTest.cpp.o
  ld: 1 duplicate symbols
  ```
- **원인**: `HttpResponseTest.cpp`의 `std::vector<ResponseCase> cases`와 `HttpRequestParserTest.cpp`(07-13)의 `std::vector<HeaderCase> cases`가 **둘 다 네임스페이스 스코프 non-const 전역**이라 external linkage. 같은 실행 파일에 링크되며 이름이 겹침. 두 파일 모두 `CaseNameGenerator`도 정의해 조용한 ODR 위반이 함께 잠복(in-class 정의라 weak 심볼로 하드 에러는 미발생).
- **해결**: 테스트 전용 전역을 고유 이름 `ResponseCases`/`ResponseNameGenerator`로 명명해 충돌 회피.
- **교훈**: 2-1과 동종 문제. 테스트 파일의 파라미터 테이블·name generator도 번역 단위 전역이므로, 익명 namespace로 감싸거나 파일별 고유 이름을 준다.

### 2-4. 🔴 `INSTANTIATE_TEST_SUITE_P` 누락 — 파라미터화 테스트가 실행 안 됨

- **증상**: 링크를 고쳐도 `TEST_P(ResponseTest, ...)`에 케이스가 하나도 주입되지 않음. 정의한 `ResponseCases`/name generator가 어디서도 사용되지 않고, 최신 GoogleTest는 인스턴스화 안 된 `TEST_P`를 `GoogleTestVerification.UninstantiatedParameterizedTestSuite`로 실패 처리.
- **원인**: value-parameterized 구성에서 `TEST_P`만 작성하고 `INSTANTIATE_TEST_SUITE_P`를 빠뜨림.
- **해결**: `INSTANTIATE_TEST_SUITE_P(Normal, ResponseTest, testing::ValuesIn(ResponseCases), ResponseNameGenerator())` 추가. 07-13 파서 테스트(`HttpRequestParserTest.cpp:96`)와 동일한 패턴.
- **교훈**: `TEST_P`는 `INSTANTIATE_TEST_SUITE_P`가 있어야 비로소 케이스가 돈다. 둘은 짝이며, 하나만 있으면 "0건 실행" 또는 verification 실패로 나타난다.

---

## 3. 빌드 · 검증

### 3-1. 신규/변경 TU 단독 컴파일 (macOS, Apple clang)

epoll에 의존하지 않는 TU만 로컬에서 확인.

```bash
clang++ -std=c++17 -Iinclude -c src/http/HttpResponse.cpp -o /dev/null
clang++ -std=c++17 -fsyntax-only -Iinclude src/connection.cpp
```

```
HttpResponse.cpp        → 에러 0 (컴파일 OK)
connection.cpp          → epoll(<sys/epoll.h>) 외 에러 0
```

`main.cpp`/`event_loop.cpp`는 `sys/epoll.h` 부재로 전체 빌드가 차단되어(플랫폼 의존) 이번에는 미빌드 → CI 대기(07-22와 동일 상황).

### 3-2. 단위 테스트 빌드·실행

```bash
cmake --build build --target tests
./build/tests/tests --gtest_filter='*Response*'
./build/tests/tests
```

```
=== HttpResponse만 ===
[ RUN      ] Normal/ResponseTest.ResponseTest/ResponseTest_200  [ OK ]
[ RUN      ] Normal/ResponseTest.ResponseTest/ResponseTest_400  [ OK ]
[  PASSED  ] 2 tests.

=== 전체 단위 테스트 ===
[==========] 23 tests from 6 test suites ran.
[  PASSED  ] 23 tests.
```

파라미터가 이름표(`ResponseTest_200`/`ResponseTest_400`)와 함께 실제 주입됨을 확인(2-4 해소), 링크 성공(2-3 해소). 기존 파서 테스트 회귀 없음.

### 3-3. `ctest` — 통합 테스트 제외 확인

```bash
ctest --test-dir build --output-on-failure
```

```
23/24 통과. 실패 1건: integration.idle_cpu (30s Timeout)
```

`integration.idle_cpu`는 epoll 기반 서버를 실제 구동하는 통합 테스트로, macOS 환경 이슈이며 이번 `HttpResponse` 작업과 무관하다. 단위 테스트(23건)는 전부 통과.

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | `HttpResponse` 커버리지 보강 | 빈 `headers`·빈 body(`Content-Length: 0`), 틀린 `Content-Length` 지정 시 skip 동작(현재 테스트는 올바른 값만 넣어 가드를 실질 검증 못 함), 커스텀 헤더 다수의 순서 보존 케이스 추가 |
| 🟡 | 미지 status code 처리 | `reason_phrase()` default가 `""` → `HTTP/1.1 500 \r\n`처럼 reason 없이 trailing space. 현재 200/400만 사용해 무해하나 확장 시 정비 필요 |
| ⚪ | `<utility>` 직접 포함 | `HttpResponse.h`가 `std::pair`를 쓰면서 `<vector>`/`<string>`의 전이 포함에 의존(07-12·07-22의 include 순서 의존과 동종) |
| ⚪ | Linux 링크 검증 | `HttpResponse` 편입 후 `http_server` 전체 링크는 epoll 부재로 로컬 미확인 → `gcc:12`/CI에서 확인 |

---

## 5. 핵심 교훈 요약

1. **번역 단위 내부 심볼은 링키지를 좁힌다.** helper 함수(2-1)든 테스트 파라미터 테이블(2-3)이든 익명 namespace/`static`/고유 이름으로 두지 않으면 파일이 늘 때 링커 충돌·조용한 ODR 위반으로 돌아온다.
2. **body에서 파생되는 헤더는 직렬화 지점의 단일 책임으로 둔다.** `Content-Length`를 호출부에 맡기면 값 불일치·중복의 여지가 생긴다. 자동 산출 + 중복 skip 가드를 함께 둔다 (2-2).
3. **`TEST_P`와 `INSTANTIATE_TEST_SUITE_P`는 짝이다.** 하나만 있으면 "0건 실행" 또는 verification 실패로 나타나며, 통과 로그가 아니라 실행된 케이스 이름으로 확인해야 한다 (2-4).
