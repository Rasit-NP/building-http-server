# 작업 로그 인덱스

`building-http-server`의 일자별 작업 & 트러블슈팅 문서 목록.
새 문서 추가 시 **최신 항목이 위로** 오도록 한 줄씩 등록한다. (규칙: [`../CLAUDE.md`](../CLAUDE.md), 양식: [`_TEMPLATE.md`](_TEMPLATE.md))

- [2026-06-05] graceful-shutdown — `SIGINT`/`SIGTERM` 핸들러 + `accept()`→`std::optional<Socket>`로 graceful shutdown 구현. idle 종료는 정상, 활성 연결 중 종료는 미보장(향후 과제) ([문서](2026-06-05-graceful-shutdown.md))
- [2026-06-02] socket-refactoring — `main.cpp` echo 로직을 RAII `Socket` 클래스로 분리, 빌드 통과 ([문서](2026-06-02-socket-refactoring.md))
