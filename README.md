# mymydb

A tiny relational database engine in C11, following [DESIGN.md](DESIGN.md).
It splits query processing into **parser → optimizer → executor**, exactly as
the design outlines, and stores everything in memory (row arrays).

## Build & run

```sh
make          # produces ./mymydb
make run      # build + start the REPL
make test     # build + run the test suite
make bench    # build + run the benchmark (./build/run_bench [N])
make memcheck # run the test suite under valgrind (leaks = failure)
make clean
```

Requires `gcc` (or `clang`) on x86-64 Linux; `make memcheck` also needs `valgrind`.

## Usage

Interactive REPL, or pipe SQL via stdin:

```sh
./mymydb <<'SQL'
CREATE TABLE users (id INT, name TEXT, age INT);
INSERT INTO users VALUES (1, 'alice', 30);
INSERT INTO users VALUES (2, 'bob', 25);
SELECT name, age FROM users WHERE age > 25 AND name != 'bob';
SQL
```

### Supported SQL (v0)

- `CREATE TABLE t (col TYPE, ...)` — types: `INT`, `TEXT`
- `INSERT INTO t VALUES (v1, v2, ...)` — positional, in column order
- `SELECT * | col, ... FROM t [WHERE <cond>]`
- WHERE conditions: `= != < <= > >=`, combined with `AND` / `OR` and
  parentheses. Left side is a column, right side a literal.

Text literals use single quotes; embed a quote by doubling it (`'it''s'`).

### REPL meta-commands

```
.tables            list tables
.schema [table]    show CREATE TABLE for one/all tables
.help              command help
.exit / .quit      leave
```

## Layout

Source is split by responsibility (v1):

| Directory | Role |
|-----------|------|
| `src/parse/`    | Tokenizer, AST, recursive-descent parser, parse arena |
| `src/optimize/` | Plan builder (v0: always full scan) |
| `src/execute/`  | Full scan + WHERE filter + projection |
| `src/storage/`  | In-memory tables/rows and cell values (INT / TEXT) |
| `src/cli/`      | REPL driver (`main.c`) |
| `tests/`        | Zero-dependency test suite (`make test`) |
| `bench/`        | Benchmark runner (`make bench`) |

The library objects (everything under `src/` except `cli/main.c`) are linked
into the CLI, the test binary, and the benchmark alike.

## Testing & stability

`make test` runs correctness cases: CRUD round-trips, every WHERE operator,
projection, error handling, integer-overflow rejection, string escaping, and
scale checks (100k-row insert + full-scan/filtered select).

`make memcheck` runs the whole suite under valgrind with
`--errors-for-leak-kinds=all` and fails the build on any leak. The parser
allocates each statement's AST from a single arena (`src/parse/arena.*`), so a
parse error (e.g. a malformed query or an out-of-range literal) releases the
half-built tree in one shot instead of leaking — the suite is valgrind-clean.

## Scope & limits (v0 engine)

Per the design, the optimizer always chooses a full scan; there are no indexes,
no persistence (the DB lives only for the process), no `UPDATE`/`DELETE`/`JOIN`,
and no networking yet. NULLs compare as non-matching in `WHERE`.
