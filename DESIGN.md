# 간단한 관계형 데이터페이스 서버/클라이언트

## 사용 언어 / 컴파일 환경
- 언어: C (C11 표준)
- 컴파일러: gcc (clang도 호환)
- 빌드: Makefile
- 대상 환경: 범용 리눅스 환경(x86-64) 

## 진행 방향
- parser / optimizer / executor를 나눈다.

## v0
- 파싱: SELECT/INSERT/CREATE TABLE, WHERE까지.
- 저장: 인메모리 테이블 (행 배열)
- executor: full scan + WHERE 필터
- 옵티마이저: 없음 (무조건 full scan)

## v1 (완료)
- 테스트 프로그램을 만들고 케이스를 여러개 작성할 것 (overflow, 대용량 select, 대용량 insert 등)
  → `tests/` (`make test`). overflow / 100k insert / 대용량·필터 select / 에러 경로 등
- 벤치마크 테스트 프로그램을 실행시킬 수 있는 벤치마크 프로그램을 만들것
  → `bench/` (`make bench [N]`). insert 처리량, full scan, WHERE 선택도별 처리량 측정
- 용도별로 디렉터리를 분리할 것 (parse, optimize, execute 같이)
  → `src/{parse,optimize,execute,storage,cli}/`
- 메모리 누수 등 프로그램 안정성 테스트를 같이 포함
  → `make memcheck` (valgrind, leak=실패). 파서를 arena 기반으로 바꿔 에러 경로 누수 제거

## v1.1 (완료)
- 실행마다 결과를 git 커밋 멘트로 사용할 수 있도록 한글/영어로 정리할 것 (version 디렉터리를 만들고 거기에 내용 저장)
- git을 직접 commit, merge 하지 않을 것
- DESIGN.md 파일은 이후 프롬프트 추정을 위해 수정하지 않을것


## v1.2 (완료)
- I/O에 page 아키텍처를 이용함 1page 사이즈는 4K로 한다.
- COUNT, SUM, AVG, MIN, MAX 윈도우 함수를 추가한다.
- ORDER BY 기능을 추가한다.

## v1.3
- cli 버퍼가 비어있는 상태에서 enter 키 입력시 mymydb> 형태의 프롬프트를 출력한다.