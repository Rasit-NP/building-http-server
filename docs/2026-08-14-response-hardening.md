# response-hardening — 작업 & 트러블슈팅 로그

- **날짜**: 2026-08-14
- **작업**: 08-11 남은 과제였던 `ifstream` 실패 미검사(404 부재)·`Content-Type` 부재·`StaticFileHandler` 테스트 부재를 처리하고, `HttpResponse`에 `Date` 헤더를 추가. `Date` 도입으로 `serialize()`가 wall clock에 의존하게 되자 시각을 주입받는 오버로드로 분리해 테스트 결정론성을 회복
- **결과**: Ubuntu 22.04 컨테이너 전체 빌드 성공(변경 TU 경고 0)·`ctest` **48/48 통과**(2.11s, 34 → 48). mutation testing 4종 **4/4 검출**. 기동 컨테이너에서 경로·MIME 매트릭스 8종 및 `Date` 헤더 실측. `ctest -j8` 병렬 실행은 임시 디렉터리 충돌로 3~6건 실패(미해소, 4장 🟡)

---

## 1. 작업 개요

08-11 로그 4장에 남긴 세 항목을 한 번에 처리했다. `ifstream` 실패 미검사는 같은 날 2-5(`Dockerfile`의 `public/` 미포함)를 은폐한 장본인이기도 했다 — 파일을 못 열어도 `200 / Content-Length: 0`이 나가는 바람에 배포 설정 문제가 "서버가 이상하다"로 보였다.

| 항목 | Before | After |
|------|--------|-------|
| 없는 파일 | `200 / 0B` (빈 파일과 구별 불가) | `404` + 404 HTML body |
| 디렉터리 요청 | `200 / 0B` | `404` (`fs::is_directory` 검사) |
| traversal | `400`(08-11 임시) | `404` (존재 여부를 노출하지 않음) |
| `Content-Type` | 없음 (브라우저 MIME sniffing 의존) | 확장자 기반 매핑, 미등록 확장자는 `application/octet-stream` |
| 확장자 대소문자 | — | `std::transform` + `std::tolower`로 정규화 후 조회 |
| `Date` 헤더 | 없음 | `serialize()`가 RFC 7231 IMF-fixdate 형식으로 출력 |
| `serialize()` 시그니처 | `serialize() const` 1개 | `serialize() const`(위임) + `serialize(std::time_t) const`(본체) |
| `reason_phrase()` | 200 / 400 | 404 `"Not Found"` 추가 |
| `StaticFileHandler` 커버리지 | 0건 | 14건 (`ctest` 34 → 48) |
| 테스트 document root | — | `fs::temp_directory_path()` 아래 테스트 소유 디렉터리(CWD 비의존) |

### 설계 — traversal도 404로 응답한다

08-11에는 traversal을 `400 Bad Request`로 처리하고 `// 이후에 404로 수정` 주석을 남겼다. 이번에 404로 통일했다. 400은 "요청 자체가 잘못됐다"를 뜻해 클라이언트에게 "그 경로는 형식이 특별하다"는 힌트를 준다. 없는 파일과 같은 404로 응답하면 document root 밖 파일의 **존재 여부 자체를 노출하지 않는다.**

### 설계 — `Date`를 위해 시각을 파라미터로 되돌린다

`HttpResponse`는 값 객체이고 `serialize()`는 원래 순수 함수였다. 같은 입력 → 같은 출력이므로 전문 비교가 성립했다. `Date`를 넣는 순간 함수가 외부 상태(wall clock)를 읽게 되고, 그 즉시 전문 비교가 불가능해진다. 세 가지를 검토했다.

1. **시각 주입 오버로드** — `serialize(std::time_t)`를 본체로 두고 무인자 버전은 `serialize(std::time(nullptr))`로 위임.
2. **`Date` 생성 책임을 호출자로 이동** — `HttpResponse`를 순수 직렬화기로 유지하고 `Connection`이 `headers`에 넣는다.
3. **테스트에서 `Date` 줄만 제외하고 비교** — 코드 무수정.

1번을 채택했다. 프로덕션 동작(항상 `Date` 전송)을 유지하면서 오버로드 하나로 결정론이 회복되고, 새 추상화(clock interface·mock)가 필요 없다. 2번은 `HttpResponse`가 시계를 읽지 않는다는 점에서 설계상 매력적이지만 응답 생성 지점이 늘면 `Date` 누락이 실제 결함으로 남는다. 3번은 `Date` 형식 자체를 영영 검증하지 못한다. 부수 효과로 `Date` 포맷이 테스트 범위에 들어와, locale 의존 위험(4장 ⚪)이 회귀로 감지된다.

### 설계 — 테스트 fixture는 비-템플릿 base + `base_`/`root_` 2단 구조

`TestWithParam<T>`는 파라미터 타입이 다르면 별개 클래스라 200 케이스(`StaticFileHandlerCase`)와 MIME 케이스(`MIMETestCase`)가 fixture를 직접 공유할 수 없다. 공통 헬퍼(`SetUpRoot`/`TearDownRoot`/`write_file`/`get`)를 비-템플릿 `StaticFileHandlerTester`에 두고 두 fixture가 다중 상속하는 구조로 풀었다. gtest 훅과 충돌하지 않도록 이름을 `SetUpRoot`/`TearDownRoot`로 달리한다.

traversal을 검증하려면 **document root 밖에 실재하는 파일**이 필요하다. root를 곧장 임시 디렉터리로 잡으면 그 파일이 `/tmp` 최상위에 놓여 정리 범위를 벗어난다(2-12 3라운드). 테스트가 소유하는 `base_`를 한 단계 두고 그 **안에서** `root_`를 잡으면 secret 파일은 root 밖이면서 정리 범위 안에 들어온다.

```
base_ = temp_directory_path() / unique_name()   ← 테스트 소유, TearDown에서 remove_all
root_ = base_ / unique_name()                   ← document root
base_ / "secret" / "secret.txt"                 ← root 밖이지만 정리됨
```

---

## 2. 트러블슈팅

### 2-1. 🔴 `std::transform` 인자 부족 — 컴파일 실패

- **증상**: 빌드 차단.
  ```
  src/http/StaticFileHandler.cpp:65:5: error: no matching function for call to 'transform'
  note: candidate function template not viable: requires 4 arguments, but 3 were provided
  1 error generated.
  ```
- **원인**: `std::transform(first, last, result, op)`의 **output iterator를 빠뜨려** 3-arg로 호출했다. in-place 변환이라 result가 없어도 될 것 같지만 그런 오버로드는 없다.
- **해결**: `std::transform(ext.begin(), ext.end(), ext.begin(), ...)` — 출력 대상을 자기 자신으로 명시.
- **교훈**: in-place 알고리즘도 STL에서는 출력 위치를 명시한다. `std::for_each`와 달리 `std::transform`은 "쓰는 곳"이 파라미터다.

### 2-2. 🔴 `Date` 무조건 추가로 기존 `HttpResponseTest` 2건 파괴

- **증상**: `ResponseTest_200`/`_400` 두 케이스 모두 불일치. `serialize()`를 실제로 링크해 출력을 확인:
  ```
  --- actual ---
  HTTP/1.1 200 OK
  Connection: close
  Date: Fri, 14 Aug 2026 06:02:08 GMT
  Content-Length: 7

  Success
  --- end ---
  matches existing test expectation: NO
  ```
- **원인**: 테스트가 `EXPECT_EQ(res.serialize(), c.expected)`로 **전문**을 비교하는데 `expected`에 `Date` 줄이 없다. 게다가 `Date`는 호출마다 값이 바뀌므로 expected에 하드코딩하는 것 자체가 불가능하다. 테스트가 잘못된 것이 아니라 `serialize()`가 비결정론적이 된 사실이 드러난 것이다.
- **해결**: `serialize(std::time_t now)` 오버로드를 본체로 두고 무인자 버전은 위임. 테스트는 `res.serialize(1786690411)`로 고정 시각을 주입하고 expected에 `Date: Fri, 14 Aug 2026 06:53:31 GMT`를 명시해 전문 비교를 유지.
- **교훈**: **순수 함수에 외부 상태를 들이면 "테스트가 깨진다"가 아니라 "비결정성이 드러난다".** 테스트를 느슨하게 푸는 것(`Date` 줄 제외, 정규식 매칭)은 증상을 덮을 뿐이고 검증력도 함께 잃는다. 외부 상태는 입력으로 되돌려 함수를 다시 순수하게 만드는 쪽이 우선이다.

### 2-3. 🔴 `HttpResponse.h`가 `<ctime>` 미포함 — gcc에서 빌드 차단

- **증상**: macOS(clang/libc++)에서는 전체 통과하는데 gcc에서 컴파일 실패.
  ```
  === HttpResponse.h standalone (gcc-12 / libstdc++) ===
  include/http/HttpResponse.h:15:27: error: 'std::time_t' has not been declared
  exit=1

  === -Wall -Wextra -Wpedantic on changed sources ===
  In file included from include/http/StaticFileHandler.h:6,
                   from src/http/StaticFileHandler.cpp:1:
  include/http/HttpResponse.h:15:27: error: 'std::time_t' has not been declared
  exit=1
  ```
- **원인**: 헤더가 `std::time_t`를 쓰는데 `<utility>`/`<vector>`/`<string>`만 include한다. 세 가지가 겹쳐 발견이 늦었다.
  1. **libc++는 통과한다.** `<string>`이 `<ctime>`을 transitive하게 끌어와 macOS 로컬 검사로는 절대 잡히지 않는다.
  2. **`HttpResponse.cpp`도 통과한다.** 이 TU는 `<ctime>`(3행)을 `"http/HttpResponse.h"`(4행)보다 **먼저** include하므로 우연히 성립한다.
  3. 실패하는 것은 `StaticFileHandler.cpp` 하나뿐이다. `StaticFileHandler.h` → `HttpResponse.h` 순으로 들어가 `<ctime>`이 아직 없다.
- **해결**: `HttpResponse.h`에 `# include <ctime>` 추가.
- **교훈**: **헤더는 자기가 쓰는 타입을 스스로 include해야 한다(self-contained header).** "구현 파일에 이미 있으니 됐다"는 include 순서 의존이고, 순서가 다른 TU 하나가 생기면 즉시 깨진다. 07-12에 `HttpRequest.h`가 `<cctype>`를 포함 순서에 의존한다고 남긴 과제와 **동종 재발**이다. 그리고 08-11 2-4(clang만 잡은 `-Wdefaulted-function-deleted`)와 방향만 반대인 같은 교훈 — 이번엔 gcc만 잡았다.

### 2-4. 🟡 `mime_from_extension()`을 정의만 하고 호출하지 않음 — `Content-Type` 여전히 부재

- **증상**: 빌드·테스트 모두 통과하지만 200 응답에 `Content-Type`이 붙지 않았다. `ext`를 계산해 놓고 어디에도 쓰지 않는 상태. 이번 작업의 목적 절반이 동작하지 않았다.
- **원인**: 함수 작성과 호출부 연결을 별개 단계로 진행하다 후자를 누락. **컴파일러가 경고하지 않은 것이 핵심**이며, 그 이유는 2-5에 있다.
- **해결**: `res.headers.emplace_back("Content-Type", mime_from_extension(ext));` 추가.
- **교훈**: "정의했다"와 "쓰인다"는 다른 사건이다. 미사용 코드는 컴파일러가 잡아 줄 수 있는 종류인데, 링키지 설정에 따라 그 도움을 스스로 차단할 수 있다.

### 2-5. 🟡 두 헬퍼가 external linkage — 미사용 감지가 무력화됨

- **증상**: `mime_from_extension()`이 한 번도 호출되지 않는데(2-4) `-Wall -Wextra -Wpedantic`에서 경고가 0건.
- **원인**: `make_not_found()`/`mime_from_extension()`이 익명 namespace 밖에 있어 external linkage다. 컴파일러는 "다른 TU에서 쓸 수도 있다"고 판단해 침묵한다. 실제 컴파일(`-c`)로 대조하면 차이가 분명하다:
  ```
  === 현재 (external linkage) ===
  exit=0                                    ← 경고 없음

  === 익명 namespace로 감쌌다면 ===
  /tmp/anon.cpp:16:13: warning: 'std::string {anonymous}::mime_from_extension(const std::string&)'
    defined but not used [-Wunused-function]
  ```
  `-fsyntax-only`로는 재현되지 않는다. `-Wunused-function`은 codegen 단계에서 나오므로 `-c`로 실제 컴파일해야 한다.
- **해결**: 미해소. 익명 namespace 이관은 후속으로 남겼다(4장 🟡).
- **교훈**: **내부 링키지는 스타일이 아니라 진단 품질의 문제다.** 익명 namespace로 감쌌다면 2-4가 컴파일 시점에 경고로 드러났다. 08-02 2-1(`reason_phrase()`), 08-10(`parse_content_length`)에 이어 **세 번째 재발**이며, 같은 저장소의 `HttpResponse.cpp`는 이미 `namespace { }`를 써 컨벤션도 어긋난다.

### 2-6. 🟡 `.jpg`/`.jpeg` 앞의 `.` 누락 — 절대 매치되지 않는 분기

- **증상**: MIME 테이블에 항목은 있는데 `.jpg` 파일이 `application/octet-stream`으로 나간다.
  ```cpp
  if (ext == "jpg" || ext == "jpeg")  return "image/jpeg";   // 선행 '.' 없음
  ```
- **원인**: `fs::path::extension()`은 **항상 선행 `.`을 포함해** 반환한다(`.jpg`). 따라서 `"jpg"`와의 비교는 어떤 입력에서도 참이 될 수 없다. 나머지 5개 항목은 `.`이 있어 정상 동작하므로 테이블을 훑어봐도 눈에 띄지 않았다.
- **해결**: `ext == ".jpg" || ext == ".jpeg"`.
- **교훈**: **조용히 틀리는 테이블은 테스트로만 잡힌다.** 한 항목만 형식이 다르면 육안 검토를 통과한다. 이 건은 뒤이어 만든 `MIMETest`가 확장자별 케이스를 두면서 회귀로 고정됐다.

### 2-7. 🟡 `<cctype>` 미포함

- **증상**: `std::tolower`를 쓰는데 include가 없다. 빌드는 통과.
- **원인**: transitive include 의존.
- **해결**: `# include <cctype>` 추가.
- **교훈**: 2-3과 동일한 종류이며, 그쪽은 표준 라이브러리가 달라지자 실제로 빌드가 깨졌다.

### 2-8. 🟡 `http_date()`의 파라미터가 non-const lvalue reference

- **증상**: 컴파일·동작 모두 정상이나 리뷰에서 지적. `std::string http_date(std::time_t& now)`.
- **원인**: `gmtime_r`이 `const std::time_t*`를 받으므로 참조로 받을 이유가 없는데 관성으로 참조를 썼다. 호출자에게 "이 함수가 `now`를 수정한다"는 잘못된 신호를 주고, rvalue를 받지 못해 `http_date(std::time(nullptr))` 같은 직접 호출이 컴파일되지 않는다.
- **해결**: `const std::time_t&`로 한정.
- **교훈**: 참조는 "수정한다" 또는 "복사가 비싸다"는 신호다. 스칼라에 대해 둘 다 아니면 값으로 받는다.

### 2-9. 🟡 무인자 `serialize()`가 미검증 — 자기 비교 assertion의 초 경계 race

- **증상**: 오버로드 분리 직후, 프로덕션에서 실제로 쓰이는 무인자 `serialize()`를 검증하는 테스트가 없었다. 이 오버로드를 통째로 지워도 테스트가 전부 통과하는 상태.
- **원인**: 테스트가 `serialize(고정시각)`만 호출하므로 위임이 실제로 일어나는지 확인할 경로가 없다.
- **해결**: `EXPECT_EQ(res.serialize(), res.serialize(std::time(nullptr)));` 추가.
- **교훈**: 다만 이 assertion에는 **두 `std::time()` 호출 사이에 초 경계가 걸리면 실패하는 창**이 남는다. 5초 루프 실측:
  ```
  첫 불일치 발생 (iteration 901295)
  5초간 5918669회 실행, 불일치 2회
  ```
  실행당 실패 확률 약 **3.4e-7**로 실무상 거의 발생하지 않아 현 형태를 유지했다. 다만 "결정론을 회복하려고 만든 구조에 비결정적 assertion을 다시 들인" 셈이라, 재현 불가능한 CI 실패가 한 번이라도 관측되면 `Date` 줄 제외 비교로 바꾸는 것이 옳다(4장 🟡).

### 2-10. 🔴 `MIMETest`에 `INSTANTIATE_TEST_SUITE_P` 누락 — 8케이스 0건 실행

- **증상**: MIME 테스트가 실행 결과에 나타나지 않고 대신 별도 실패가 뜬다.
  ```
  [ RUN      ] GoogleTestVerification.UninstantiatedParameterizedTestSuite<MIMETest>
  Parameterized test suite MIMETest is defined via TEST_P, but never instantiated.
  None of the test cases will run.
  [  FAILED  ] GoogleTestVerification.UninstantiatedParameterizedTestSuite<MIMETest>
  ```
  `MIMETestCases`(8건)와 `MIMETestNameGenerator`를 정의해 뒀지만 어디에도 연결되지 않은 상태였다.
- **원인**: `TEST_P`는 케이스 **본문**만 정의한다. 파라미터 공급은 `INSTANTIATE_TEST_SUITE_P`가 담당하며, 이것이 없으면 본문이 한 번도 실행되지 않는다.
- **해결**: `INSTANTIATE_TEST_SUITE_P(MIMETest, MIMETest, testing::ValuesIn(MIMETestCases), MIMETestNameGenerator());` 추가.
- **교훈**: **08-02 2-4와 동일한 재발.** 다행히 gtest 1.10+ 는 이를 **조용한 통과가 아니라 명시적 실패**로 보고한다 — 이 안전망이 없었다면 "MIME 테스트를 추가했다"고 믿은 채 커버리지가 0인 상태로 남았을 것이다.

### 2-11. 🔴 `DirectoryTargetReturns404`가 선행 `/` 누락으로 다른 분기를 탐

- **증상**: 테스트는 통과하는데, `fs::is_directory` 검사를 **통째로 제거해도 그대로 통과**한다.
- **원인**: `get("subdir")`에 선행 `/`가 없었다. `handle()`은 `target.substr(1)`로 첫 글자를 무조건 버리므로 `"ubdir"`가 되고, 존재하지 않는 경로라 `!file.is_open()` 분기에서 404가 나온다. `is_directory` 검사는 한 번도 실행되지 않았다. 다른 테스트는 전부 `"/" + name` 형태라 이 케이스만 형식이 달랐다.
- **해결**: `get("/subdir")`. mutation을 다시 적용하니 즉시 검출된다.
  ```
  C++ exception with description "basic_filebuf::underflow error reading the file:
    Is a directory" thrown in the test body.
  [  FAILED  ] StaticFileHandlerTest.DirectoryTargetReturns404
  ```
- **교훈**: 이 실패 메시지가 부수적으로 알려준 사실 — **`is_directory` 검사는 단순한 404 분기가 아니라 예외를 막는 방벽이다.** `ifstream`은 디렉터리에 대해 `is_open()`이 성공하고 읽기 시점에 던지므로, 검사가 없으면 예외가 `handle()` 밖으로 전파된다.

### 2-12. 🔴 `TraversalReturns404`가 네 라운드에 걸쳐 잔존 — 매번 다른 이유로 통과

가장 중요한 테스트인데 **네 번 연속으로 "통과하지만 아무것도 검증하지 않는" 상태**였다.

- **증상 (공통)**: `inside`(traversal) 검사를 제거해도 통과. 즉 root 밖 파일이 서빙되는데 초록불이 켜진다.
- **원인 (1차) — 검증 대상 파일이 없음**: `get("/../secret.txt")`만 호출하고 `secret.txt`를 만들지 않았다. 파일이 없으므로 차단 여부와 무관하게 404다.
- **원인 (2차) — `ofstream`이 flush되지 않음**: 파일 생성을 추가했으나 `ofs`가 스코프에 살아 있는 채로 `get()`을 호출했다. `"SECRET"`은 스트림 버퍼에만 있고 디스크는 0바이트. 진단 출력:
  ```
  [진단] status=200  body="" (len=0)
  [진단] 디스크상 secret.txt 내용="" (len=0)
  ```
  **`status=200`** 이 핵심이다 — traversal이 실제로 성공해 root 밖 파일이 서빙됐는데, 파일이 비어 있어 `EXPECT_NE(res.body, "SECRET")`가 통과했다.
- **원인 (3차) — 단언이 부정형**: 위와 같은 이유로 `EXPECT_NE`는 "파일이 비었으면 무조건 통과"한다. 통과 경로가 지나치게 넓다.
- **원인 (4차) — 디렉터리 생성 경로와 파일 기록 경로의 기준 불일치**: `create_directories(root_ / "secret")`(root **안**)와 `ofstream(root_.parent_path() / "secret" / "secret.txt")`(root **밖**)가 서로 다른 곳을 가리켰다. 부모 디렉터리가 없어 `ofstream`이 **조용히 실패**한다.
  ```
  [진단] root_                  = /tmp/sfh test_0
  [진단] create_directories 대상 = /tmp/sfh test_0/secret -> 존재:1
  [진단] ofstream 기록 대상      = /tmp/secret/secret.txt -> 존재:0
  [진단] 그 부모 디렉터리        = /tmp/secret -> 존재:0
  ```
- **해결**: 네 가지를 모두 반영. `base_`/`root_` 2단 구조(1장)로 정리 범위를 확보하고, `create_directories(base_ / "secret")`로 기준을 통일하고, `ofstream`을 스코프 블록으로 감싸 flush를 강제하고, `EXPECT_EQ(res.status_code, 404)`를 주 단언으로 추가했다.
  ```
  :124: Failure   Which is: 200
  :125: Failure
  [  FAILED  ] StaticFileHandlerTest.TraversalReturns404
  ```
- **교훈**: 세 가지가 겹친다.
  1. **부정형 단언(`EXPECT_NE`)은 통과 경로가 넓다.** 계약을 직접 확인하는 긍정형(`EXPECT_EQ(status, 404)`)을 주 단언으로 둔다. `status` 검사 하나면 flush 문제(2차·3차)까지 함께 잡혔다.
  2. **`ofstream`은 소멸 시점에 flush된다.** 같은 스코프에서 파일을 읽는 코드를 호출하면 아직 디스크에 없다. 파일 생성은 스코프 블록으로 닫는다.
  3. **네 번 다른 이유로 통과한 근본 원인은 셋업이 헬퍼를 우회한 것이다.** 다른 테스트는 전부 `write_file()`/`get()`을 거치는데 이것만 raw `ofstream`과 `parent_path()`로 경로를 직접 조립했다. 08-11 2-3(traversal 가드가 세 결함 중첩으로 세 라운드 잔존)과 **구조가 같다** — 그때는 구현이, 이번엔 테스트가 같은 함정에 빠졌다.

### 2-13. 🟡 lowercase 변환이 미검증 — 모든 케이스가 이미 소문자

- **증상**: MIME 테스트 8건이 모두 통과하는데, `std::transform` + `std::tolower`를 **통째로 제거해도 전부 통과**한다.
  ```
  ### mutation: lowercase 변환 제거
  [  PASSED  ] 11 tests.
  ```
- **원인**: `MIMETestCases`의 확장자가 전부 소문자(`html`/`css`/…)라 변환이 no-op이다. 실행되지만 아무 일도 하지 않는 코드를 통과시키고 있었다.
- **해결**: 케이스 테이블을 늘리지 않고 테스트 본문에서 대문자 이름을 파생시켜 두 번 검증(`html.html`과 `html.HTML`). mutation 검출:
  ```
  [  FAILED  ] MIMETest_html / _css / _js / _png / _jpg / _jpeg / _ico   (7건)
  ```
  `gif`만 살아남는 것은 정상이다 — fallback 케이스라 대소문자와 무관하게 `application/octet-stream`이다.
- **교훈**: **입력이 전부 한쪽에 몰려 있으면 정규화 코드는 실행돼도 검증되지 않는다.** 08-10 🟡 1번(청크 1 고정 급여로 clamp 분기가 원리적으로 미실행)과 동종의 사각이며, 이번에도 mutation testing이 아니었으면 드러나지 않았다.

### 2-14. 🟡 `ctest -j` 병렬 실행에서 임시 디렉터리 충돌 — 미해소

- **증상**: 순차 실행은 14/14 통과하는데 `ctest -j8`은 매 회 실패한다.
  ```
  === 순차 ===
  100% tests passed, 0 tests failed out of 14
  === ctest -j8 4회 ===
  57% tests passed, 6 tests failed out of 14
  71% tests passed, 4 tests failed out of 14
  79% tests passed, 3 tests failed out of 14
  57% tests passed, 6 tests failed out of 14
  ```
  ```
  filesystem error: cannot remove all: No such file or directory
    [/tmp/sfh test_0] [/tmp/sfh test_0/ico.ico]
  ```
- **원인**: `unique_name()`이 프로세스 **내** 카운터(`static std::atomic<int>`)만 쓴다. `gtest_discover_tests`는 테스트마다 별도 프로세스를 띄우므로 모든 프로세스가 counter=0에서 시작해 `/tmp/sfh test_0`을 공유한다. 한 프로세스의 `TearDown`이 다른 프로세스가 쓰는 디렉터리를 지운다.
- **해결**: 미해소(4장 🟡). 이름에 `::getpid()`를 섞으면 된다.
- **교훈**: `base_`/`root_` 2단 구조 도입 후 실패 건수가 **0~4건에서 3~6건으로 늘었다.** `TearDownRoot()`가 `root_`가 아니라 `base_`를 지우게 되면서 한 번의 충돌이 상대 프로세스의 **작업 트리 전체**를 날리기 때문이다. **격리와 정리는 별개의 축이며, 한쪽만 개선하면 다른 쪽 결함이 증폭될 수 있다.**

---

## 3. 빌드 · 검증

### 3-1. 전체 빌드 · `ctest` (Ubuntu 22.04 컨테이너, CI 환경 기준)

```bash
docker run --rm -v "$PWD":/src -w /src ubuntu:22.04 bash -c '
  apt-get update -qq && apt-get install -y -qq build-essential cmake git
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
  g++ -std=c++17 -Iinclude -Wall -Wextra -Wpedantic -c src/http/HttpResponse.cpp     -o /tmp/a.o
  g++ -std=c++17 -Iinclude -Wall -Wextra -Wpedantic -c src/http/StaticFileHandler.cpp -o /tmp/b.o
  cd build && ctest'
```

```
=== gcc ===
g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0

=== 빌드 ===
빌드 exit=0
경고 1건: src/event_loop.cpp:113:33: warning: enumerated and non-enumerated type
          in conditional expression [-Wextra]          ← 기존 건(06-15부터 존재)
HttpResponse.cpp: 경고 0
StaticFileHandler.cpp: 경고 0

=== ctest ===
100% tests passed, 0 tests failed out of 48
Total Test time (real) =   2.11 sec
```

08-11의 34건에서 14건 증가(200 응답 3 + 404 3 + MIME 8). `tests/CMakeLists.txt`에 `handlers` 라이브러리를 추가하고 `tests` 타깃에 링크했다 — 08-11 2-1(CMake 소스 목록 누락으로 링크 실패)의 재발을 피하려 소스 작성과 등록을 한 묶음으로 처리했다. 2-3 해소 전에는 `StaticFileHandler.cpp`가 `'std::time_t' has not been declared`로 막혀 빌드 자체가 되지 않았다.

### 3-2. 경로 · MIME 매트릭스 (기동 컨테이너 실측)

`public/`에 `style.css`(6B)와 `pic.PNG`(랜덤 100B, **대문자 확장자**)를 추가해 검증했다.

```bash
cd /app && ./http_server 8080 &
for p in / /index.html /style.css /pic.PNG /nope.png /../etc/passwd "/index.html?v=1" /subdirnope; do
  printf '%-20s -> ' "$p"
  curl -s -o /dev/null --path-as-is \
    -w '%{http_code} %{content_type} %{size_download}B\n' --max-time 3 "http://127.0.0.1:8080$p"
done
curl -sI http://127.0.0.1:8080/ | grep -i '^date'
```

```
/                    -> 200 text/html 151B     ✅ index.html 매핑
/index.html          -> 200 text/html 151B     ✅
/style.css           -> 200 text/css 6B        ✅ MIME 매핑
/pic.PNG             -> 200 image/png 100B     ✅ 대문자 확장자 정규화
/nope.png            -> 404 text/html 48B      ✅ 없는 파일 (08-11에는 200 0B)
/../etc/passwd       -> 404 text/html 48B      ✅ traversal 차단 (08-11에는 400)
/index.html?v=1      -> 404 text/html 48B      ⚠️ query string 미분리 (4장 🟡)
/subdirnope          -> 404 text/html 48B      ✅

Date: Fri, 14 Aug 2026 10:02:07 GMT            ✅ RFC 7231 IMF-fixdate
```

`--path-as-is`가 필수다. 없으면 curl이 클라이언트 쪽에서 `..`를 정규화해 traversal 검증이 무의미해진다(08-11 3-3과 동일).

08-11 대비 달라진 지점은 넷이다. 없는 파일이 `200 0B` → `404`, traversal이 `400` → `404`, 모든 응답에 `Content-Type`, 그리고 `Date` 헤더.

### 3-3. mutation testing — 테스트 실효성 검증

"통과한다"와 "검증한다"는 다른 문제이므로, 구현의 각 검사를 하나씩 제거해 대응 테스트가 실패하는지 확인했다.

```bash
# 구현에서 검사 한 개를 제거 → 재빌드 → 해당 테스트가 실패하는지 확인
python3 mutate.py <dir|traversal|mime|lower>
cmake --build build --target tests -j
./build/tests/tests --gtest_filter='StaticFileHandlerTest.*:MIMETest*'
```

**최종 결과 — 4/4 검출:**

| 제거한 검사 | 검출한 테스트 |
|---|---|
| `inside` (traversal) | `TraversalReturns404` |
| `fs::is_directory` | `DirectoryTargetReturns404` |
| `.html` → `text/WRONG` | `MIMETest_html` |
| lowercase 변환 | `MIMETest_html/css/js/png/jpg/jpeg/ico` (7건) |

**작업 중간 결과 — 0/3 검출** (2-11·2-12·2-13 해소 전):

```
mutation: is_directory 검사 제거     → [  PASSED  ] 6 tests.
mutation: inside(traversal) 검사 제거 → [  PASSED  ] 6 tests.
mutation: ".html" → "text/WRONG"     → [  PASSED  ] 6 tests.
```

**세 검사를 통째로 들어내도 테스트가 전부 통과했다.** 각각 원인이 달랐고, 그 진단이 2-11~2-13이다.

> 검증 중 한 차례, mutation 루프에서 소스만 복원하고 **재빌드를 하지 않아** 변형된 바이너리로 `ctest`를 돌린 적이 있다. 그 측정치는 폐기하고 클린 재빌드 후 재측정했다. mutation 실험은 매 라운드 소스 복원 → 재빌드 → 측정 순서를 지켜야 한다.

### 3-4. `Date` 고정 시각 주입 확인

테스트에 쓸 timestamp는 `date -u` / `date +%s`로 취득하고 `http_date()`와 동일한 `strftime` 포맷으로 렌더링해 expected 문자열과 짝을 맞췄다.

```
2026-08-14 06:53:31 UTC / 1786690411
→ strftime 렌더링: "Fri, 14 Aug 2026 06:53:31 GMT"
```

`std::mktime`은 `tm`을 **로컬 타임존으로 해석**하므로 쓰지 않았다. 동일 `tm` 필드에 TZ만 바꿔 실측하면 결과가 갈린다:

```
[host default TZ]      t=1786678496   -> Fri, 14 Aug 2026 03:34:56 GMT
[TZ=UTC]               t=1786710896   -> Fri, 14 Aug 2026 12:34:56 GMT
[TZ=America/New_York]  t=1786725296   -> Fri, 14 Aug 2026 16:34:56 GMT
```

로컬에서는 통과하고 CI(보통 UTC)에서 깨지는 테스트가 된다. 결정론을 위해 시각을 주입해 놓고 다시 환경 의존을 들이는 셈이라, 캘린더 날짜에서 `time_t`를 만들 때는 `timegm`(UTC 기준)을 써야 한다.

### 3-5. HEAD 요청 — raw socket 대조

```
### HEAD / -- body를 보내는가?
  total=253 bytes, body=151 bytes, server_closed=True
  | [body 40] b'<!DOCTYPE html>\n<html lang="en">\n<head>\n'

### GET /  -- 비교용
  total=253 bytes, body=151 bytes, server_closed=True
  | [body 40] b'<!DOCTYPE html>\n<html lang="en">\n<head>\n'
```

두 응답이 **바이트 단위로 동일**하다. RFC 7230 §3.3은 HEAD 응답에 body를 보내지 않을 것을 요구한다(`Content-Length`는 GET과 같은 값으로 유지). 이번 범위 밖이라 4장에 남겼다.

### 3-6. 병렬 실행 (미해소 항목의 실측)

```bash
cd build
ctest -R 'StaticFileHandler|MIME'          # 순차
for i in 1 2 3 4; do ctest -j8 -R 'StaticFileHandler|MIME'; done
```

결과는 2-14에 기재. 순차 14/14 통과, `-j8`은 3~6건 실패(비결정적).

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | `unique_name()`에 PID 미포함 | 2-14의 원인. `"sfh_test_" + std::to_string(::getpid()) + "_" + counter`로 해소된다. 현재 `ctest -j`를 쓸 수 없다 |
| 🟡 | HEAD 응답에 body 전송 | 3-5 실측. RFC 7230 §3.3 위반. 현재는 매 응답 후 연결을 닫아 증상이 드러나지 않지만, keep-alive 도입 시 클라이언트가 body를 다음 응답의 시작으로 오인한다. 08-11 남은 과제 "method 미검사(405)"와 함께 처리 |
| 🟡 | 두 헬퍼 external linkage | 2-5 참조. 익명 namespace 이관 필요. 08-02·08-10에 이은 세 번째 동종 재발 |
| 🟡 | query string 미분리 | 3-2 실측대로 `/index.html?v=1`이 404. `HttpRequestParser.cpp:130`이 request-target 전체를 `path`에 담는다. 정적 서버로서 실사용에 바로 걸린다 |
| 🟡 | `Date` 중복 방지 없음 | `serialize()`는 사용자 헤더에서 `Content-Length`만 skip하고 `Date`는 걸러내지 않는다. 호출자가 `headers`에 `Date`를 넣으면 두 번 나간다. 08-02에 `Content-Length`로 세운 dedup 원칙이 `Date`에는 미적용 |
| 🟡 | `gmtime_r` 반환값 미확인 | 실패 시 zero-init된 `tm`이 그대로 `strftime`에 들어가 잘못된 날짜가 나간다(UB는 아님) |
| 🟡 | 자기 비교 assertion의 초 경계 race | 2-9 참조. 실행당 약 3.4e-7. `Date` 줄 제외 비교로 결정론화 가능 |
| 🟡 | 테스트 timestamp가 매직 넘버 | `HttpResponseTest.cpp:49`의 `1786690411`과 expected의 `Date:` 줄 사이에 연결 고리가 없다. 이름 붙인 상수 + 렌더링 결과 주석 필요 |
| 🟡 | `ifstream`을 `is_directory` 검사보다 먼저 생성 | 현재 순서는 `ifstream` → `is_directory` → `is_open`. 2-11에서 확인했듯 `is_directory`가 예외를 막는 유일한 방벽이므로, 검사를 먼저 두면 의도가 분명해진다 |
| 🟡 | `StaticFileHandler(const std::string root)` | by-value 파라미터의 `const`는 호출자에게 의미가 없고 `root_(root)`에서 move를 막아 복사를 강제한다. `std::string root` + `root_(std::move(root))`가 맞다(08-11 남은 과제가 오히려 악화) |
| 🟡 | `StaticFileHandler handler();` dead declaration | fixture의 멤버 **변수**가 아니라 멤버 **함수 선언**(most vexing parse). 정의도 호출도 없어 링크는 통과하지만 fixture가 handler를 보유한다고 오해하게 한다 |
| 🟡 | `MissingFildReturns404` 오타 · 테스트 `<atomic>` 미포함 | 전자는 `Fild` → `File`. 후자는 2-3·2-7과 동종이며, PID 방식으로 바꾸면 `std::atomic` 자체가 불필요해진다 |
| 🟡 | 미검증으로 남은 동작 | `/` → `/index.html` 치환, 404 응답의 body·`Content-Type`, 빈 파일의 `Content-Length: 0`, `Date` 포맷의 경계값(한 자리 일자 zero-padding, 월 경계) |
| 🟡 | `Connection: close` 비대칭 | `connection.cpp:20`에서 `close_after_write = true`로 닫으면서 헤더는 에러 경로(`:28`)에만 붙인다. HTTP/1.1 기본은 persistent라 클라이언트가 재사용을 시도한다(08-11에서 이월) |
| ⚪ | percent-decoding 없음 | `%20`이 포함된 파일명을 열 수 없다. 도입 시 **디코딩을 traversal 검사보다 먼저** 해야 한다. 순서가 바뀌면 `%2e%2e`로 현재 방어가 뚫린다(08-11에서 이월) |
| ⚪ | `strftime`의 `%a`/`%b`가 locale 의존 | 현재 "C" locale이라 IMF-fixdate와 일치하지만 `setlocale(LC_TIME, ...)`이 들어오면 스펙 위반. 현 테스트가 회귀는 감지한다. `gmtime_r`도 POSIX 확장으로 `<ctime>` 보장 밖 |
| ⚪ | MIME 테이블이 함수 내 if 체인 | 항목이 늘면 `unordered_map` 등이 적절. 현재 7종 |
| ⚪ | 테스트 전역 변수 external linkage | `handler_cases_200`/`MIMETestCases`/`unique_name`이 익명 namespace 밖. 08-02 2-3(`cases` 전역의 `duplicate symbol` 충돌)의 재발 자리 |
| ⚪ | `INSTANTIATE_TEST_SUITE_P(MIMETest, MIMETest, ...)` · 수동 대문자 산술 | 전자는 prefix와 suite 이름 중복으로 테스트 이름이 3중 반복. 후자는 `ch - 32` 대신 `static_cast<char>(std::toupper(...))`가 구현 쪽 `std::tolower`와 대칭 |

---

## 5. 핵심 교훈 요약

1. **순수 함수에 wall clock을 들이면 테스트가 깨지는 게 아니라 비결정성이 드러난 것이다.** 테스트를 느슨하게 푸는 대신 외부 상태를 입력으로 되돌린다. 오버로드 하나로 프로덕션 동작을 유지한 채 결정론을 회복했다 (2-2).
2. **헤더는 자기가 쓰는 타입을 스스로 include한다.** "구현 파일에 이미 있다"는 include 순서 의존이고, 순서가 다른 TU 하나가 생기면 즉시 깨진다. 그리고 표준 라이브러리마다 transitive include가 달라 libc++는 통과하고 libstdc++는 실패했다 — 08-11에 clang만 잡은 진단이 있었듯 이번엔 gcc만 잡았다 (2-3).
3. **내부 링키지는 스타일이 아니라 진단 품질이다.** `mime_from_extension()` 미호출을 컴파일러가 잡지 못한 이유가 external linkage였고, 익명 namespace였다면 `-Wunused-function`이 즉시 알려줬다. 08-02·08-10에 이은 세 번째 재발 (2-4, 2-5).
4. **"통과한다"와 "검증한다"는 다른 사건이다.** 처음 작성한 6건은 전부 초록불이었지만 mutation testing으로 확인하니 `is_directory`·`inside`·MIME 세 검사를 통째로 들어내도 통과했다. 08-10에 이어 두 번째로 mutation testing이 테스트 사각을 드러냈다 (3-3).
5. **부정형 단언은 통과 경로가 넓다.** `EXPECT_NE(body, "SECRET")`은 파일이 비기만 해도 통과한다. 계약을 직접 확인하는 `EXPECT_EQ(status, 404)`가 주 단언이어야 하고, 실제로 이 한 줄이 flush 결함까지 함께 잡았다 (2-12).
6. **셋업이 헬퍼를 우회하면 같은 실수가 반복된다.** traversal 테스트만 raw `ofstream`과 `parent_path()`로 경로를 직접 조립했고, 네 라운드에 걸쳐 매번 다른 이유로 통과했다. 08-11 2-3에서 구현이 겪은 것과 구조가 같다 (2-12).
7. **입력이 한쪽에 몰리면 정규화 코드는 실행돼도 검증되지 않는다.** 소문자 확장자만 있는 테이블에서 lowercase 변환은 no-op이다. 08-10의 clamp 분기 미실행과 동종 (2-13).
8. **격리와 정리는 별개의 축이다.** 정리 범위를 `root_`에서 `base_`로 넓힌 개선이 격리 결함(공유 디렉터리 이름)의 피해를 0~4건에서 3~6건으로 키웠다 (2-14).
