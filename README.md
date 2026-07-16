# mymydb

A tiny relational database engine in C11, following [DESIGN.md](DESIGN.md).
It splits query processing into **parser → optimizer → executor**, exactly as
the design outlines, and stores rows in fixed-size **4KB pages** in memory.

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

### Supported SQL

- `CREATE TABLE t (col TYPE, ...)` — types: `INT`, `TEXT`
- `INSERT INTO t VALUES (v1, v2, ...)` — positional, in column order
- `SELECT * | col, ... FROM t [WHERE <cond>] [ORDER BY col [ASC|DESC], ...]`
- Aggregates (v1.2): `SELECT COUNT(*), COUNT(col), SUM(col), AVG(col),
  MIN(col), MAX(col) FROM t [WHERE <cond>]` — a query is either plain columns
  or aggregates, not a mix. `SUM`/`AVG` require an `INT` column; `MIN`/`MAX`
  work on `INT` (numeric) or `TEXT` (lexicographic).
- `ORDER BY` (v1.2): one or more keys, each optionally `ASC` (default) or
  `DESC`; `NULL`s sort first. Keys may be columns that are not projected.
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
| `src/optimize/` | Plan builder (full scan; resolves projection/aggregates/ORDER BY) |
| `src/execute/`  | Full scan + WHERE filter + projection + aggregates + ORDER BY |
| `src/storage/`  | 4KB slotted-page tables and cell values (INT / TEXT) |
| `src/cli/`      | REPL driver (`main.c`) |
| `tests/`        | Zero-dependency test suite (`make test`) |
| `bench/`        | Benchmark runner (`make bench`) |

The library objects (everything under `src/` except `cli/main.c`) are linked
into the CLI, the test binary, and the benchmark alike.

## Testing & stability

`make test` runs correctness cases: CRUD round-trips, every WHERE operator,
projection, aggregates (COUNT/SUM/AVG/MIN/MAX + empty-set semantics), ORDER BY
(ASC/DESC, multi-key, NULLs-first), page round-trips (multi-page spans +
oversized-row rejection), error handling, integer-overflow rejection, string
escaping, and scale checks (100k-row insert + full-scan/filtered select).

`make memcheck` runs the whole suite under valgrind with
`--errors-for-leak-kinds=all` and fails the build on any leak. The parser
allocates each statement's AST from a single arena (`src/parse/arena.*`), so a
parse error (e.g. a malformed query or an out-of-range literal) releases the
half-built tree in one shot instead of leaking — the suite is valgrind-clean.

## Storage: 4KB pages (v1.2)

Rows are serialized into fixed-size 4KB **slotted pages** (`src/storage/`)
rather than a flat array of live `Value` structs. Each page keeps a small
header, a slot directory growing from the front, and tuple data growing from
the back; a scan deserializes tuples through a cursor. A single row must fit in
one page (oversized rows are rejected). This is the I/O access path the design
calls for, kept in memory.

## Scope & limits

Per the design, the optimizer always chooses a full scan; there are no indexes,
no `GROUP BY`, no persistence (the DB lives only for the process), no
`UPDATE`/`DELETE`/`JOIN`, and no networking yet. NULLs compare as non-matching
in `WHERE`, and sort first under `ORDER BY`.
