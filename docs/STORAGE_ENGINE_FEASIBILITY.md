# Feasibility: srf-db as a Pluggable Storage Engine for DB25

> Status: **Feasibility study / design note.** Written from the DB25 side of the
> interface. It analyzes what DB25 requires of *any* pluggable storage engine and
> where srf-db would attach, then flags the srf-db-specific unknowns that decide
> the verdict.

## 0. Scope of this analysis

This note was produced with read access to the two DB25 components that were
open at the time: the **SQL tokenizer** and the **semantic analyzer**. The
umbrella `DB25` repo (an HTAP prototype) and `db25-sql-parser` were read from
their *public* documentation. Three things that materially affect the verdict
were **not** readable and are treated as unknowns, called out explicitly below:

- **srf-db itself** — a private C repository. Its data model, type system, API
  surface, concurrency and transaction semantics are inferred, not observed.
- **`db25-logical-plan`** — the planner, private and created concurrently with
  this study. The logical→physical→access-method contract lives (or will live)
  here.
- **The umbrella `DB25` source** — only its public README/architecture summary
  was available.

Where a claim depends on one of these, it is marked **(unverified)**.

## 1. The DB25 pipeline and where a storage engine attaches

DB25 is a decomposed C++23 pipeline of independently-versioned components:

```
SQL text
  → tokenizer            (SIMD lexer, zero-copy string_view tokens)
  → db25-sql-parser      (arena-allocated ASTNode tree; ParserMode::Production)
  → semantic analyzer    (name resolution + type inference, Catalog-injected)
  → db25-logical-plan    (logical plan; cost-based)            (unverified)
  → physical plan        (PhysicalPlanner)                     (unverified)
  → execution engine     (iterator-based + vectorized)         (unverified)
  → storage / access methods  ← srf-db would live here
```

A "pluggable storage engine" attaches at **two** distinct seams, not one, and
they are at very different levels of maturity in the readable code:

| Seam | What the engine supplies | Maturity in readable code |
|------|--------------------------|---------------------------|
| **Catalog / metadata** | table & column schema, types, ids, constraints, (ideally) statistics | **Exists as a stable abstraction today** (`semantic::Catalog`) |
| **Access methods / data path** | scans, index probes, tuple/batch iterators, mutations, transactions | **Implied only** — present in the umbrella prototype, not yet a formal interface in the modular repos (unverified) |

This split is the central finding: **the metadata seam is real and easy; the
data seam is not yet a defined contract.**

## 2. Seam A — Catalog / metadata (concrete, available now)

The analyzer depends on exactly one injectable abstraction
(`include/db25/semantic/catalog.hpp`):

```cpp
class Catalog {
public:
    virtual ~Catalog() = default;
    [[nodiscard]] virtual const TableInfo* find_table(std::string_view name) const = 0;
};
```

with

```cpp
struct ColumnInfo {
    std::string name;
    DataType    type = DataType::Unknown;
    bool        nullable = true;
    bool        has_default = false;   // suppresses NOT NULL violations on INSERT
    std::uint32_t column_id = 0;       // stable within the owning table
};
struct TableInfo {
    std::string  name;
    std::uint32_t table_id = 0;
    std::vector<ColumnInfo> columns;
};
```

**A srf-db-backed `Catalog` is implementable immediately and cheaply.** Any
engine that can enumerate `(table → columns, each with a type, nullability, a
default flag, and stable ids)` can implement `find_table`. This lets the DB25
analyzer resolve names and type-check queries against live srf-db schemas with
no changes to DB25.

Two obligations fall on srf-db here:

1. **Type mapping (the tightest coupling).** Every srf-db physical column type
   must map onto `ast::DataType`. The analyzer's inference, coercion model,
   set-operation reconciliation, and DML assignment checks are all expressed in
   these categories: `Integer`, `BigInt`, `Decimal`, `Double`, char/`varchar`/
   `Text`, `Boolean`, `Date`, `Time`, `Timestamp`, `Interval`, `Array`, `Null`,
   `Unknown`. The mapping must be **lossless in both directions** for the round
   trip (planner reads the type, executor materializes values) — precision-bearing
   types (`Decimal`, `Timestamp`, `Interval`) are where a C storage engine's
   representation most easily diverges. **(unverified: srf-db's type system.)**

2. **Stable identity.** `table_id` / `column_id` are integer identities the rest
   of the pipeline keys on (they are written back into the AST's
   `context.analysis` union and into `ResolvedColumn`). Either srf-db owns the
   authoritative catalog ids and DB25 adopts them, or DB25's catalog layer
   assigns and maintains a stable srf-db↔DB25 id map. Both work; the choice
   should be made deliberately, because these ids flow all the way to the
   executor.

**Verdict for Seam A: feasible now, low effort** — a thin adapter class. This is
the recommended first integration milestone; it is independently valuable
(schema-aware analysis over srf-db) even before any data flows.

## 3. Seam B — Access methods / data path (the real gating work)

The umbrella `DB25` architecture already names the capabilities a storage engine
must expose to the executor **(all unverified against current modular source)**:

- **Dual layout** — row-oriented (OLTP) and column-oriented (OLAP).
- **Scan operators** — sequential scan and **index scan with filter pushdown**.
- **Join feeds** — tuples/batches consumable by nested-loop and hash joins.
- **Iterator-based, vectorized execution** — the executor wants batches, not
  one-tuple-at-a-time calls, or the per-call overhead dominates.
- **`ComputationalStorageInterface`** — near-data processing (push filter/aggregate
  down to the device/engine). This is DB25's stated differentiator.

None of this yet exists as an *abstract, documented `StorageEngine` interface* in
the repos that were readable — there is no vtable, no header, defining
`open_scan / next_batch / index_lookup / insert / begin_txn`. It is a prototype
capability in the monolith and, presumably, being shaped inside the just-created
`db25-logical-plan`. **Consequently, the feasibility of srf-db at this seam is
gated primarily by a DB25-side decision that has not been finalized: what the
access-method contract *is*.** srf-db cannot be judged "pluggable" against an
interface that is not yet fixed.

What srf-db will need to provide once that contract exists:

| Requirement | Why DB25 needs it | srf-db status |
|-------------|-------------------|---------------|
| Table scan cursor | feeds every physical plan | **(unverified)** |
| Index scan + **predicate pushdown** | umbrella lists it as a first-class scan op | **(unverified)** |
| Projection pushdown | avoid materializing unread columns (esp. OLAP) | **(unverified)** |
| **Batch/columnar output** | vectorized executor; avoids per-row C↔C++ crossings | **(unverified)** |
| Insert / update / delete / upsert | analyzer already type-checks DML | **(unverified)** |
| Constraint enforcement (NOT NULL, defaults, FK, unique/index) | `ColumnInfo` models NOT NULL + defaults; umbrella `DatabaseSchema` has `add_index`/`add_foreign_key` | **(unverified)** |
| Transactions / snapshot isolation / MVCC | HTAP = concurrent OLAP scans over live OLTP | **(unverified)** |
| Concurrency (multiple concurrent readers) | vectorized/parallel execution | **(unverified)** |
| Optimizer statistics (cardinality, histograms) | planner is **cost-based** | **(unverified)** |
| Durability (WAL) | crash safety for the OLTP side | **(unverified)** |

## 4. The C ↔ C++23 boundary

srf-db is **C**; DB25 is **C++23** (templates, SIMD, arena allocation,
`string_view` throughout). This is a normal and often *desirable* arrangement —
a C ABI is the most stable plugin boundary there is, which is exactly what
"pluggable" wants — but it has consequences:

- **Design the ABI batch-first, not tuple-first.** The DB25 executor is
  vectorized. A C API shaped as `next_row()` forces a function call and a
  type-marshal per tuple across the language boundary, which will undercut the
  vectorized engine. A `next_batch(columns, capacity, out_count)` shape (Arrow-
  like column buffers, or DB25's native column format) keeps the crossing rate
  low and preserves cache behavior. **This is the single most important design
  choice** if srf-db is to serve the OLAP half of HTAP well.
- **Zero-copy is partially lost.** DB25's parser/tokenizer get their speed from
  `string_view` into arena memory. Values crossing from srf-db are owned by
  srf-db; DB25 must borrow (with a clear lifetime contract) or copy. Define
  ownership explicitly in the ABI (who frees, when a buffer is valid until).
- **Type marshalling must be total.** Every `ast::DataType` needs a defined C
  encoding, including null representation (DB25 tracks nullability as a 2-bit
  field; the ABI needs an explicit null bitmap or sentinel).

None of these is a blocker; they are ABI-design requirements.

## 5. HTAP fit

DB25 is explicitly HTAP (OLTP + OLAP, workload-routed). That raises the storage
bar beyond a single-model store:

- The OLAP path wants **columnar batches + snapshot reads** that don't block
  writers. If srf-db is a row-oriented, single-writer OLTP store, it can serve
  the transactional half well but DB25 would still need a columnar/analytical
  path (a second engine, a column cache, or a columnar mode in srf-db).
- If srf-db can expose **MVCC snapshots** and **near-data filter/aggregate**
  pushdown, it aligns unusually well with DB25's differentiators and becomes a
  strong first engine rather than merely a viable one. **(unverified.)**

## 6. Verdict and recommended path

**Feasible, and architecturally well-matched at the metadata seam; the data seam
is feasible but gated on DB25 first defining its storage-engine contract — a
DB25-side task, not an srf-db limitation.**

Recommended phasing:

- **Phase 0 — Catalog adapter (do now, low risk).** Implement
  `SrfDbCatalog : public semantic::Catalog`. Map srf-db schema → `TableInfo`/
  `ColumnInfo`, settle the type mapping and the id-ownership question. Delivers
  schema-aware analysis over srf-db immediately and forces the type-mapping
  decision early, where it is cheap.
- **Phase 1 — Define the access-method interface (DB25-side, blocking).** Land an
  abstract `StorageEngine` header in the modular tree (likely alongside
  `db25-logical-plan`): scan open/next-batch, index lookup with pushed predicate,
  projection pushdown, mutation, txn lifecycle, stats. Until this exists, "srf-db
  as a pluggable engine" has no interface to plug into.
- **Phase 2 — srf-db access shim.** Implement the Phase-1 interface over srf-db's
  C API with a **batch-first ABI**. Start read-only (scan + index + pushdown),
  then DML.
- **Phase 3 — Full HTAP.** Transactions/MVCC snapshots, cost statistics, columnar/
  analytical path, and — if srf-db supports it — `ComputationalStorageInterface`
  near-data pushdown.

## 7. Decisive open questions for srf-db (needed to firm up the verdict)

1. **Data model** — relational tables? row, column, or both? or key-value that
   DB25 layers relations over?
2. **Type system** — enumerate srf-db types and their lossless mapping to
   `ast::DataType` (esp. `Decimal`, `Timestamp`, `Interval`, `Array`).
3. **Access API** — is there a cursor/scan API today? predicate/projection
   pushdown? secondary indexes?
4. **Transactions & concurrency** — MVCC? snapshot isolation? concurrent readers?
   single-writer?
5. **Statistics** — can it produce cardinality/histogram estimates for the
   cost-based planner?
6. **Near-data compute** — can it push down filters/aggregates
   (`ComputationalStorageInterface`)?
7. **C ABI & memory ownership** — stability guarantees, and who owns/frees value
   buffers across the boundary.
8. **Durability** — WAL / crash-recovery model.

Answering §7 turns each **(unverified)** above into a concrete yes/no and
converts this study into an implementation plan.
