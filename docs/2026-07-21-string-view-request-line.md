# string-view-request-line — 작업 & 트러블슈팅 로그

- **날짜**: 2026-07-21
- **작업**: request line(`method`/`path`/`version`)을 `std::string` 복사에서 `buffer_`를 가리키는 `std::string_view`(zero-copy)로 전환
- **결과**: macOS 로컬 파서 테스트 `tests` 21/21 통과 (경고 0)

---

## 1. 작업 개요

`HttpRequest`의 request line 3필드를 소유 문자열에서 `buffer_` 내부를 가리키는 view로 바꿔,
`parseRequestLine()`의 `substr` 복사를 제거하는 리팩터링.

| 항목 | Before | After |
|------|--------|-------|
| `HttpRequest::method`/`path`/`version` | `std::string` (복사 소유) | `std::string_view` (`buffer_` 참조) |
| request line 토큰화 | `request_line = buffer_.substr(0, crlf)` 임시 복사 후 `find`/`substr` | `buffer_` 위에서 직접 `find`, offset/length만 계산 |
| 위치 저장 | 없음 | `method_off_/len_`, `path_off_/len_`, `version_off_/len_` 멤버 6종 |
| `string_view` 대입 시점 | — | `State::Done` (dangling 회피 목적) |
| request line 검증 | `parseRequestLine()`에서 `isValid()`/`isValidVersion()` | 빈 토큰은 `parseRequestLine()`에서 length로, version 형식은 `Done`에서 |

### 설계 원칙 — 대입은 왜 `Done` 이후인가
view는 `buffer_.data()` 를 가리킨다. 파싱 진행 중에는 후속 `parse()` 호출의 `buffer_.append()`가
재할당을 일으켜 view가 dangling될 수 있으므로, **요청이 완결되는 `State::Done`에서 대입**한다.
단, "빈 토큰" 같은 request line 자체의 구조적 검증은 완결과 무관하므로 `parseRequestLine()` 시점에 수행한다.

---

## 2. 트러블슈팅 <!-- 발생 순서대로 -->

### 2-1. 🔴 대입 전 검증 — 빈 `string_view`를 검사해 전 요청이 `Error`
- **증상**: 정상 요청이 전부 실패. `method`/`path`/`version` 모두 `""`, 반환값 `Error`. 파서 테스트 8건 실패.
- **원인**: `string_view` 대입은 `State::Done`에서 하는데, `isValid()`/`isValidVersion()` 검증은 그 이전 `parseRequestLine()`에서 실행. 검증 시점의 view는 아직 default(빈 상태)라 `isValid()`가 항상 false → 즉시 `Error`.
- **해결**: `string_view` 대입과 두 검증을 모두 `State::Done`으로 이동(대입 → 검증 순서 확정).
- **교훈**: view 기반 필드는 "언제 유효 데이터를 가리키는가"가 계약이다. 필드를 읽는 모든 코드(검증 포함)는 대입 이후에 와야 한다.

### 2-2. 🔴 세 번째 공백 검사의 `crlf` 경계 누락 → 헤더 공백을 오인
- **증상**: 헤더가 있는 정상 요청만 실패(`POST /x`+`Host`, `GET /path`+`Host`/`User-Agent`), 헤더 없는 요청은 통과. 3건 실패.
- **원인**: 여분 공백 검사가 `buffer_.find(' ', sp2+1)` 로 **버퍼 전체**를 검색. `sp1`/`sp2`에는 `>= crlf` 경계가 있으나 이 검사에는 없어, 헤더의 공백(`Host: a:8080`)을 "request line에 공백 3개"로 오인 → `Error`. (개선 시도로 `if (sp3 != npos || sp3 < crlf)` 를 넣었으나 `||`라 `sp3 != npos`와 동치가 되어 경계가 무력화 — 동일 증상 지속.)
- **해결**: `size_t sp3 = buffer_.find(' ', sp2 + 1); if (sp3 != std::string::npos && sp3 < crlf)` — `&&`로 request line 범위 안에서만 판정.
- **교훈**: `substr`로 만든 부분 문자열 위 검색을 원본 버퍼 검색으로 바꿀 때는 **모든** 탐색에 상한(`crlf`)을 함께 옮겨야 한다. 경계 조건은 `&&`/`||`까지 포함해 검증한다.

### 2-3. 🔴 빈 토큰 조기 판정 상실 → 불완전 요청이 `Incomplete` 반환
- **증상**: `"GET  HTTP/1.1\r\n"`(공백 2개, path가 빈 토큰)이 기대값 `Error` 대신 `Incomplete`. `ErrorTest_EmptyToken` 1건 실패.
- **원인**: dangling 회피를 위해 검증을 `State::Done`으로 옮긴 결과, 빈 토큰(`path_len_ == 0`) 판정도 `Done`으로 밀림. 이 입력엔 종료 `\r\n\r\n`이 없어 `parseHeaders()`가 `Incomplete`를 반환 → `Done` 미도달 → 검증 자체가 실행되지 않음.
- **해결**: request line 구조 검증(빈 토큰)은 view 대입과 무관하므로 `parseRequestLine()`으로 되돌림 — `if (!method_len_ || !path_len_ || !version_len_) { Error }`. length는 대입 없이 즉시 알 수 있어 dangling과 무관하게 조기 판정 가능. version 형식 검증은 `Done`에 유지.
- **교훈**: 검증을 뒤로 미룰지는 "그 검증이 무엇에 의존하는가"로 결정한다. length 기반 구조 검증은 view 대입에 의존하지 않으므로 완결 전에 둘 수 있다.

---

## 3. 빌드 · 검증

`sys/epoll.h` 의존 대상(`http_server` 서버 바이너리, `integration.*`)은 macOS에 헤더가 없어 제외.
파서 정적 라이브러리와 파서 테스트 실행 파일(`tests`)만 빌드·실행.

```bash
cmake --build cmake-build-debug --target tests
./cmake-build-debug/tests/tests
```

```
[==========] 21 tests from 5 test suites ran. (0 ms total)
[  PASSED  ] 21 tests.
```

- 파서 TU(`HttpRequestParser.cpp`) `-Wall -Wextra -Wpedantic` 경고 0.
- 실패 이력: 8 → 3 → 3 → 1 → 0 (트러블슈팅 2-1 → 2-2 → 2-3 순으로 해소).

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | `string_view` 수명 계약 | view는 `buffer_` 재할당(`parse()` 재호출) 시 dangling. keep-alive/pipelining 확장 시 `request()` 소비 시점 계약을 명시 필요 |
| ⚪ | offset 멤버 정리 | request line은 첫 파싱에서만 계산되므로 offset/length 6종을 멤버 대신 지역 변수로 둘 수 있는지 재검토 |
| ⚪ | `integration.*` 검증 | `sys/epoll.h` 부재로 macOS 미실행 → Ubuntu/CI 대기 |

---

## 5. 핵심 교훈 요약

1. view 필드는 "유효 데이터를 언제 가리키는가"가 계약이다 — 검증을 포함한 모든 read는 대입 이후에 와야 한다(2-1).
2. `substr` 부분 문자열 검색을 원본 버퍼 검색으로 바꿀 때는 상한 경계를 **모든** 탐색에 함께 옮기고, `&&`/`||`까지 검증한다(2-2).
3. 검증을 뒤로 미룰지는 그 검증의 의존성으로 판단한다 — length 기반 구조 검증은 view 대입과 무관하므로 완결 전 조기 판정이 가능하다(2-3).
