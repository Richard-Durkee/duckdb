# Sketch: generalizing `TemporaryMemoryManager` → `OperatorMemoryManager`

**Status:** design sketch only — no wiring. For a maintainer design discussion before any implementation.

## The idea in one line

`TemporaryMemoryManager` (TMM) is *already* a per-operator memory abstraction — it just only tracks the **spillable** operators. Generalize it to track **all** memory-holding operators, so it becomes the single place that (v1) reports per-operator memory and (v2) can enforce per-operator budgets. This is not a new Trino-style abstraction; it is widening the one DuckDB already has.

## Why it's the right seam (not a new abstraction)

TMM today: each spillable operator (hash join, radix aggregate, sort) registers a `TemporaryMemoryState` and declares `remaining_size` (how much it would use if fully in memory). TMM dynamically grants each state a `reservation` (its in-memory budget) so the sum stays under the limit; the operator spills whatever exceeds its reservation. So TMM already:
- knows a per-operator "wanted size" and grants a per-operator budget, and
- **is** the enforcement mechanism for spillable operators (reservation < remaining_size ⇒ spill).

What it does *not* do: track operators that don't spill (nested-loop join RHS, in-memory window/CTE materialization, IN-list hashes). Their memory is invisible to TMM's arbitration.

## A real bug this fixes (independent of metrics)

Because non-spillable memory is invisible to TMM, **TMM over-grants**: it hands spillable operators reservations as if the non-spillable operators' memory weren't competing for the same pool. Tracking non-spillable usage in the same manager makes the arbitration budget-accurate. This is a correctness motivation that stands on its own, separate from observability or enforcement — useful for getting maintainer buy-in.

## Should it be renamed? Yes.

"Temporary" means *spill-to-temp-directory*. Once the manager also tracks memory that never spills, the name is wrong. Proposed:

| Today | Generalized |
|---|---|
| `TemporaryMemoryManager` | `OperatorMemoryManager` |
| `TemporaryMemoryState` | `OperatorMemoryReservation` |
| `Register(context)` | `Register(context, MemoryReservationMode)` |

Migration: keep `TemporaryMemoryManager`/`TemporaryMemoryState` as deprecated `using` aliases for one release so the ~3 call sites (hash join, aggregate, sort) don't have to change in the same PR.

## The generalized interface (sketch)

The key addition is a **mode**: spillable states behave exactly as today; fixed states report usage that the manager subtracts from the pool but never asks to spill.

```cpp
enum class MemoryReservationMode : uint8_t {
    // Existing behavior: manager grants a reservation <= requested; operator spills the remainder.
    SPILLABLE,
    // New: operator holds this memory and cannot spill it. Manager tracks it against the pool
    // (so SPILLABLE grants shrink accordingly) but never grants less than requested. v2 enforcement:
    // when the pool is exhausted, a FIXED request that cannot be satisfied fails fast with a
    // per-operator OOM naming the operator, instead of a global, unattributed OOM.
    FIXED,
};

class OperatorMemoryReservation {          // was TemporaryMemoryState
public:
    // --- unchanged spillable API ---
    void SetRemainingSize(idx_t requested_size);
    void SetRemainingSizeAndUpdateReservation(ClientContext &, idx_t requested_size);
    idx_t GetReservation() const;          // granted in-memory budget
    idx_t GetRemainingSize() const;        // what the operator wants

    // --- v1 observability (both modes) ---
    // Peak of GetReservation()/usage over the state's lifetime; read by the profiler for
    // operator.peak_memory. Already prototyped for SPILLABLE via GetPeakReservation().
    idx_t GetPeakUsage() const;

    // --- v2 enforcement hook (no-op in v1) ---
    // FIXED states call this as they grow; returns false if the pool cannot admit the growth,
    // letting the operator fail fast / apply backpressure instead of OOMing the process.
    // SPILLABLE states ignore the bool and spill as today.
    bool TryReserve(ClientContext &, idx_t new_usage);

    MemoryReservationMode GetMode() const;
};

class OperatorMemoryManager {              // was TemporaryMemoryManager
public:
    static OperatorMemoryManager &Get(ClientContext &);
    unique_ptr<OperatorMemoryReservation> Register(ClientContext &, MemoryReservationMode mode);
    // ... existing arbitration internals unchanged; FIXED usage is subtracted from the pool
    //     before SPILLABLE reservations are computed ...
};
```

## Staging

- **v1 — observability (this whole thread's goal).** Add `FIXED` mode + `GetPeakUsage()`. Wire the big non-spillable operators (nested-loop join, window, materialized/recursive CTE, IN-list) to `Register(FIXED)` and report usage. The profiler reads `GetPeakUsage()` → `operator.peak_memory` for *every* tracked operator, spillable and not. Also fixes the over-grant bug. **No behavior change for spillable operators.**
- **v2 — enforcement (future, only if maintainers want it).** `TryReserve` starts returning `false`/failing fast when the pool is exhausted; FIXED operators get a per-operator OOM instead of a global one. This is purely additive on the same seam — no rewrite.

## What to ask the maintainers (the design discussion)

1. Is generalizing TMM (vs. a separate mechanism) the direction you'd accept?
2. Is the over-grant/accuracy fix motivation enough on its own, with observability as the payoff?
3. Rename now (with aliases) or keep the `TemporaryMemory*` names and just add `FIXED`?
4. Is per-operator enforcement (v2) something you'd ever want, or is reactive pool-level spilling the intended model forever? (Determines whether `TryReserve` belongs in the v1 interface at all.)

## Non-goals

- Not building v2 enforcement now. Not touching the hot allocation path (this reuses the existing register/report model, which operators call at sink granularity, not per-allocation). Not a Trino-style `OperatorContext` hierarchy — no driver/pipeline/task rollup layer; the profiler already provides the per-operator tree.
