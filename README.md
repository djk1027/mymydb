# mymydb

A tiny relational database engine in C11, following [DESIGN.md](DESIGN.md).
It splits query processing into **parser → optimizer → executor**, exactly as
the design outlines. Everything is modelled as an **object** (a logical
database or a table); rows are serialized into extent-allocated **8KB blocks**
that can be **checkpointed to disk** and reloaded.

## Build & run

```sh
make          # produces ./bin/mymydb  (binaries live under bin/, v2.2 layout)
make run      # build + start the REPL
make install  # copy the binary into <base>/bin (see base-path resolution below)
make test     # build + run the test suite
make bench    # build + run the benchmark (./build/run_bench [N])
make memcheck # run the test suite under valgrind (leaks = failure)
make clean
```

The binary is built into `bin/` so that, when the working tree is itself the
base (`$HOME/mymydb`), it already sits in `<base>/bin`. `make install` copies it
to another base's `bin/` — `$MYMY/mymydb/bin`, or `$HOME/mymydb/bin` when `$MYMY`
is unset; override with `make install BASE=/path`.

Requires `gcc` (or `clang`) on x86-64 Linux; `make memcheck` also needs `valgrind`.

## Usage

Always durable (v2.2 removed the in-memory mode). Data lives under a **base
path** holding `bin/` and `data/`; each database is its own file in `data/`:

```sh
./mymydb                 # base = $MYMY/mymydb, or $HOME/mymydb if $MYMY is unset
./mymydb /srv/mydb       # base = /srv/mydb  (created if absent, reloaded if present)
```

Base-path precedence: command-line argument > `base_path` in
[`mymy.conf`](mymy.conf) > `$MYMY/mymydb` > `$HOME/mymydb`.

DML/DDL end with `;`; meta commands (`SHOW`, `USE`, `SET`, `HELP`, `EXIT`,
`CHECKPOINT`) run on Enter without one:

```sh
./mymydb /srv/mydb <<'SQL'
CREATE DATABASE shop;
USE shop
CREATE TABLE users (id INT, name TEXT, age INT);
INSERT INTO users VALUES (1, 'alice', 30);
INSERT INTO users VALUES (2, 'bob', 25);
DELETE FROM users WHERE age < 26;
SELECT name, age FROM users WHERE age > 25 ORDER BY age DESC;
SHOW TABLES
CHECKPOINT
SQL
```

### Supported SQL

- `CREATE DATABASE <name>` / `USE <name>` (v2.0) — an instance holds many
  logical databases; a default `main` exists at startup.
- `CREATE TABLE t (col TYPE, ...)` — types: `INT`, `TEXT`
- `INSERT INTO t VALUES (v1, v2, ...)` — positional, in column order
- `UPDATE t SET col = val, ... [WHERE <cond>]` (v2.3)
- `DELETE FROM t [WHERE <cond>]` (v2.0) — omitting `WHERE` deletes every row.
- `CREATE INDEX name ON t (col, ...)` / `DROP INDEX name` (v2.3) — B+tree,
  multi-column, non-unique. The optimizer uses an index automatically for `=`
  and range predicates (see below).
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

### Commands (MySQL-style)

The old `.`-prefixed commands were removed in v2.1; meta commands are keywords
that (v2.2) run on Enter **without** a trailing `;`:

```
SHOW DATABASES              list databases
SHOW TABLES                 list tables in the current database
SHOW CREATE TABLE t         show a table's DDL
SHOW PARAMETERS             per-database parameters (name | value)
SHOW GLOBAL PARAMETERS      instance-wide parameters
SET key = value             set a per-database parameter
SET GLOBAL block_size = N   set a global parameter
CHECKPOINT                  flush all databases to disk now
HELP                        command help
EXIT  /  QUIT               leave (or Ctrl-D)
```

## Layout

Source is split by responsibility, and the storage layer is split per object
type (v2.0):

| Path | Role |
|------|------|
| `src/parse/`             | Tokenizer, AST, recursive-descent parser, parse arena |
| `src/optimize/`          | Plan builder: full/index scan choice, projection/aggregates/ORDER BY |
| `src/execute/`           | Scan + WHERE + projection + aggregates + ORDER BY + INSERT/UPDATE/DELETE |
| `src/storage/object.*`    | Object header (id + type), PageId, block/extent constants |
| `src/storage/value.*`     | Cell values (INT / TEXT) |
| `src/storage/param.*`     | Parameter store (name → value) for global/per-db params |
| `src/storage/serialize.*` | Growable byte buffer + reader for master/catalog (de)serialization |
| `src/storage/pager.*`     | Raw block device: one data file (runtime block size) |
| `src/storage/table.*`     | Table object: slotted blocks, extents, row cursor |
| `src/storage/index.*`     | B+tree index object: block nodes, insert/split, search/range/delete |
| `src/storage/database.*`  | Database object: tables + indexes + params + its own pager |
| `src/storage/dbfile.*`    | Per-database file I/O (superblock + catalog + data) |
| `src/storage/instance.*`  | Process instance: master registry + global params + databases |
| `src/cli/`                | REPL driver + `mymy.conf` loader (`main.c`) |
| `tests/`                  | Zero-dependency test suite (`make test`) |
| `bench/`                  | Benchmark runner (`make bench`) |

The library objects (everything under `src/` except `cli/main.c`) are linked
into the CLI, the test binary, and the benchmark alike.

## Testing & stability

`make test` runs correctness cases: CRUD round-trips, every WHERE operator,
projection, aggregates (COUNT/SUM/AVG/MIN/MAX + empty-set semantics), ORDER BY
(ASC/DESC, multi-key, NULLs-first), block round-trips (multi-block/extent spans
+ oversized-row rejection), `DELETE` (single/range/all + reuse after delete),
multiple databases (isolation, `USE`, one file each), persistence (save →
reopen → reload, deletes stay gone, post-reload mutations persist), `SHOW`/`SET`
and parameters (global vs per-db, seeding, validation), runtime block size (a
512-byte file reopened at a different requested size keeps its stored size),
`UPDATE` and B+tree indexes (point/range/text lookups, covering scans, index
maintenance across INSERT/UPDATE/DELETE, DROP), error handling, integer-overflow
rejection, string escaping, and scale checks (100k-row insert + full-scan/
filtered select). Each task prints its wall-clock time (v2.2). The 100k/1M
performance tests (v2.3) print full-scan vs index-scan timings and assert the
index wins.

`make memcheck` runs the whole suite under valgrind with
`--errors-for-leak-kinds=all` and fails the build on any leak. The parser
allocates each statement's AST from a single arena (`src/parse/arena.*`), so a
parse error (e.g. a malformed query or an out-of-range literal) releases the
half-built tree in one shot instead of leaking — the suite is valgrind-clean.

## Storage: objects, 8KB blocks & per-database files

Everything is an **object** with an id and a type (`OBJ_DB` or `OBJ_TABLE`).
An **instance** owns one or more logical **databases**, each owning **tables**.

Rows are serialized into fixed-size **slotted blocks** (8KB by default). Each
block keeps a small header, a slot directory growing from the front, and tuple
data growing from the back; a scan deserializes tuples through a cursor,
skipping tombstoned (`DELETE`d) slots. A single row must fit in one block
(oversized rows are rejected).

Blocks are handed out in **extents** — the first extent is 8 blocks and each
next one doubles (8, 16, 32, …). Placing each extent as a contiguous run keeps
an object's blocks LBA-contiguous for sequential scans. Every block has a unique
**page id** `(db_id, object_id, sequence)`.

**Files (v2.2).** Under `<base>/data/`, each database is a self-contained file
`<name>.mdb` (block 0 superblock + a contiguous catalog run + extent data
blocks), and a `_master` file holds the instance registry (global params,
`next_db_id`, the database list). Blocks are buffered in memory; a **checkpoint**
flushes dirty blocks + each catalog + the master. Checkpoints run on an explicit
`CHECKPOINT` and at shutdown — **not** per mutation (v2.2) — so bulk loads stay
fast. Reopening the base reloads the master and every database file.

## Parameters & config (v2.1)

Settings are **parameters** (name → value), persisted with the instance:

- **Global** parameters are instance-wide (`SET GLOBAL name = value`,
  `SHOW GLOBAL PARAMETERS`), stored in `_master`. `block_size` and `base_path`
  live here.
- **Per-database** parameters are seeded from the global defaults when a
  database is created (`SET name = value`, `SHOW PARAMETERS`), stored in that
  database's file.

The **block size** is a runtime value (default 8192, range 512..65536). It is
global and applied **only when a new database file is created**; reopening an
existing file uses the size recorded in its superblock (no re-layout).

[`mymy.conf`](mymy.conf) (read from the working directory) sets defaults with
`key = value` lines and `#` comments — `base_path` and `block_size`. A base-path
argument on the command line overrides the config's `base_path`.

## Indexes (v2.3)

A `CREATE INDEX` builds a **B+tree** whose nodes are blocks in the database file
(extent-allocated, catalogued, reloaded on restart). Keys use an order-
preserving encoding with the row locator appended, so equal values are handled
correctly across leaf splits. The optimizer drives an index from a single `=`/
range comparison — or one conjunct of a top-level `AND` chain — on the index's
first column, re-checking the full predicate per row (`OR` falls back to a full
scan). If every column the query needs is in the index, it is served from the
leaves without touching data blocks (a **covering** scan).

## Scope & limits

The optimizer chooses a full scan or a single-column-driven index scan; there is
no `GROUP BY`, no `JOIN`, and no networking yet. Indexes are non-unique (no
primary keys) and deletes never merge B+tree nodes. Durability is
checkpoint-based: changes since the last `CHECKPOINT` (or clean shutdown) are
lost on a crash — a finer-grained/WAL policy is future work. NULLs compare as
non-matching in `WHERE`, and sort first under `ORDER BY`.
