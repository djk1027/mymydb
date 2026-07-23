# mymydb

A tiny relational database engine in C11, following [DESIGN.md](DESIGN.md).
It splits query processing into **parser → optimizer → executor**, exactly as
the design outlines. Everything is modelled as an **object** (a logical
database or a table); rows are serialized into extent-allocated **8KB blocks**
that can be **checkpointed to disk** and reloaded.

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

In-memory (no persistence), or durable when given a data-file path (or a
`data_file` in [`mymy.conf`](mymy.conf)):

```sh
./mymydb              # in-memory REPL (or config's data_file, if set)
./mymydb shop.db      # durable: created if absent, reloaded if present
```

Interactive REPL, or pipe SQL via stdin (every statement ends with `;`):

```sh
./mymydb shop.db <<'SQL'
CREATE DATABASE shop;
USE shop;
CREATE TABLE users (id INT, name TEXT, age INT);
INSERT INTO users VALUES (1, 'alice', 30);
INSERT INTO users VALUES (2, 'bob', 25);
DELETE FROM users WHERE age < 26;
SELECT name, age FROM users WHERE age > 25 ORDER BY age DESC;
SHOW TABLES;
SET GLOBAL block_size = 16384;
SQL
```

### Supported SQL

- `CREATE DATABASE <name>` / `USE <name>` (v2.0) — an instance holds many
  logical databases; a default `main` exists at startup.
- `CREATE TABLE t (col TYPE, ...)` — types: `INT`, `TEXT`
- `INSERT INTO t VALUES (v1, v2, ...)` — positional, in column order
- `DELETE FROM t [WHERE <cond>]` (v2.0) — omitting `WHERE` deletes every row.
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

### Commands (MySQL-style, v2.1)

Meta commands are ordinary `;`-terminated statements (the old `.`-prefixed
commands were removed in v2.1):

```
SHOW DATABASES;              list databases
SHOW TABLES;                 list tables in the current database
SHOW CREATE TABLE t;         show a table's DDL
SHOW PARAMETERS;             per-database parameters (name | value)
SHOW GLOBAL PARAMETERS;      instance-wide parameters
SET key = value;             set a per-database parameter
SET GLOBAL block_size = N;   set a global parameter
HELP;                        command help
EXIT;  /  QUIT;              leave (or Ctrl-D)
```

## Layout

Source is split by responsibility, and the storage layer is split per object
type (v2.0):

| Path | Role |
|------|------|
| `src/parse/`             | Tokenizer, AST, recursive-descent parser, parse arena |
| `src/optimize/`          | Plan builder (full scan; resolves projection/aggregates/ORDER BY) |
| `src/execute/`           | Scan + WHERE + projection + aggregates + ORDER BY + DELETE |
| `src/storage/object.*`   | Object header (id + type), PageId, block/extent constants |
| `src/storage/value.*`    | Cell values (INT / TEXT) |
| `src/storage/param.*`    | Parameter store (name → value) for global/per-db params |
| `src/storage/pager.*`    | Raw block device: the data file (runtime block size) |
| `src/storage/table.*`    | Table object: slotted blocks, extents, row cursor |
| `src/storage/database.*` | Database object: a logical database owning tables + params |
| `src/storage/instance.*` | Process instance: databases + global params + pager + checkpoint/load |
| `src/cli/`               | REPL driver + `mymy.conf` loader (`main.c`) |
| `tests/`                 | Zero-dependency test suite (`make test`) |
| `bench/`                 | Benchmark runner (`make bench`) |

The library objects (everything under `src/` except `cli/main.c`) are linked
into the CLI, the test binary, and the benchmark alike.

## Testing & stability

`make test` runs correctness cases: CRUD round-trips, every WHERE operator,
projection, aggregates (COUNT/SUM/AVG/MIN/MAX + empty-set semantics), ORDER BY
(ASC/DESC, multi-key, NULLs-first), block round-trips (multi-block/extent spans
+ oversized-row rejection), `DELETE` (single/range/all + reuse after delete),
multiple databases (isolation, `USE`), persistence (save → reopen → reload,
deletes stay gone, post-reload mutations persist), `SHOW`/`SET` and parameters
(global vs per-db, seeding, validation), runtime block size (a 512-byte file
reopened at a different requested size keeps its stored size), error handling,
integer-overflow rejection, string escaping, and scale checks (100k-row insert
+ full-scan/filtered select).

`make memcheck` runs the whole suite under valgrind with
`--errors-for-leak-kinds=all` and fails the build on any leak. The parser
allocates each statement's AST from a single arena (`src/parse/arena.*`), so a
parse error (e.g. a malformed query or an out-of-range literal) releases the
half-built tree in one shot instead of leaking — the suite is valgrind-clean.

## Storage: objects, 8KB blocks & persistence (v2.0)

Everything is an **object** with an id and a type (`OBJ_DB` or `OBJ_TABLE`).
An **instance** owns one or more logical **databases**, each owning **tables**.

Rows are serialized into fixed-size 8KB **slotted blocks**. Each block keeps a
small header, a slot directory growing from the front, and tuple data growing
from the back; a scan deserializes tuples through a cursor, skipping tombstoned
(`DELETE`d) slots. A single row must fit in one block (oversized rows are
rejected).

Blocks are handed out in **extents** — the first extent is 8 blocks and each
next one doubles (8, 16, 32, …). Placing each extent as a contiguous run keeps
an object's blocks LBA-contiguous for sequential scans. Every block has a unique
**page id** `(db_id, object_id, sequence)`.

When the instance is opened with a **data-file path** it is durable: block 0 is
a superblock, a contiguous catalog run stores the serialized object tree, and
table data lives in the extent blocks. A **checkpoint** (run after every
successful mutation) writes dirty blocks + catalog + superblock; reopening the
file reloads the whole tree. Without a path the instance is memory-only and
checkpoints are a no-op.

## Parameters & config (v2.1)

Settings are **parameters** (name → value), persisted in the catalog:

- **Global** parameters are instance-wide (`SET GLOBAL name = value`,
  `SHOW GLOBAL PARAMETERS`). `block_size` and `data_file` live here.
- **Per-database** parameters are seeded from the global defaults when a
  database is created (`SET name = value`, `SHOW PARAMETERS`).

The **block size** is a runtime value (default 8192, range 512..65536). It is
global and applied **only when a new data file is created**; reopening an
existing file uses the size recorded in its superblock (no re-layout).

[`mymy.conf`](mymy.conf) (read from the working directory) sets defaults with
`key = value` lines and `#` comments — currently `data_file` and `block_size`.
A data-file argument on the command line overrides the config's `data_file`.

## Scope & limits

Per the design, the optimizer always chooses a full scan; there are no indexes,
no `GROUP BY`, no `UPDATE`/`JOIN`, and no networking yet. Checkpoint scheduling
is naive (one per mutation on a file-backed instance); a smarter checkpoint
policy is a future version. NULLs compare as non-matching in `WHERE`, and sort
first under `ORDER BY`.
