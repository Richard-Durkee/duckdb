//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/buffer_pool_reservation.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {

enum class BlockState : uint8_t { BLOCK_UNLOADED = 0, BLOCK_LOADED = 1 };

// Forward declarations.
class BufferPool;
class PhysicalOperator;

//! Identifies an operator instance across memory snapshots. `operator_id` is the operator's pre-order position in
//! the query profiler's tree, so snapshot entries can be joined onto the profiled plan.
struct OperatorMemoryIdentity {
	connection_t connection_id = DConstants::INVALID_INDEX;
	idx_t query_id = DConstants::INVALID_INDEX;
	idx_t operator_id = DConstants::INVALID_INDEX;
	string operator_name;
};

//! Point-in-time memory attributed to one operator instance (see BufferPool::GetOperatorMemorySnapshot).
struct OperatorMemoryInformation {
	OperatorMemoryIdentity identity;
	idx_t memory_usage_bytes;
	idx_t peak_memory_usage_bytes;
	array<idx_t, MEMORY_TAG_COUNT> memory_usage_bytes_per_tag;
};

//! Buffer-managed memory attributed to one operator instance (or to a query), shared by all of its threads
struct OperatorMemoryCounter : public enable_shared_from_this<OperatorMemoryCounter> {
	OperatorMemoryCounter(OperatorMemoryIdentity identity_p, optional_ptr<const PhysicalOperator> op_p);

	//! Same caching scheme as BufferPool::MemoryUsage: small deltas accumulate in per-CPU caches and reach the
	//! global counters (and the peak) once a cache exceeds the threshold, so peaks are exact to within
	//! CACHE_COUNT * CACHE_THRESHOLD bytes and threads of one operator rarely contend on a cache line
	static constexpr idx_t CACHE_COUNT = 64;
	static constexpr idx_t CACHE_THRESHOLD = 32 << 10;
	static constexpr idx_t TOTAL_INDEX = MEMORY_TAG_COUNT;
	using Counters = array<atomic<int64_t>, MEMORY_TAG_COUNT + 1>;

	OperatorMemoryIdentity identity;
	//! Global usage per tag, plus the total at TOTAL_INDEX
	Counters usage;
	atomic<int64_t> peak {0};
	unique_ptr<array<Counters, CACHE_COUNT>> caches;
	//! The physical operator instance this counter attributes memory to. Identity that distinguishes two
	//! operators of the same type, and the key to map this attribution onto the profiler's per-operator tree.
	optional_ptr<const PhysicalOperator> op;
	//! The query-wide total this counter rolls up into; its peak is the peak of the query's live memory
	shared_ptr<OperatorMemoryCounter> parent;
	//! Set when the owning query ends; memory still charged to a released counter belongs to the database and is no
	//! longer reported for the operator
	atomic<bool> released {false};

	void Update(MemoryTag tag, int64_t delta);
	OperatorMemoryInformation GetInformation() const;

private:
	void UpdateGlobal(idx_t index, int64_t delta);
};

struct BufferPoolReservation {
	MemoryTag tag;
	idx_t size {0};
	BufferPool &pool;
	//! The counter this reservation is charged to, captured from BufferPool::CurrentOperator() when created
	shared_ptr<OperatorMemoryCounter> owner;
	//! POC: the counter named by the QueryContext this reservation was created with
	shared_ptr<OperatorMemoryCounter> context_owner;

	BufferPoolReservation(MemoryTag tag, BufferPool &pool);
	//! A reservation attributed to `owner` (nullptr: to no operator) instead of the thread's current operator
	BufferPoolReservation(MemoryTag tag, BufferPool &pool, shared_ptr<OperatorMemoryCounter> owner);
	BufferPoolReservation(const BufferPoolReservation &) = delete;
	BufferPoolReservation &operator=(const BufferPoolReservation &) = delete;

	BufferPoolReservation(BufferPoolReservation &&) noexcept;
	BufferPoolReservation &operator=(BufferPoolReservation &&) noexcept;

	virtual ~BufferPoolReservation();

	void Resize(idx_t new_size);
	void Merge(BufferPoolReservation src);
	//! Moves this reservation's bytes from its current owner to `new_owner`
	void SetOwner(shared_ptr<OperatorMemoryCounter> new_owner);
	void SetContextOwner(shared_ptr<OperatorMemoryCounter> new_owner);

	//! The counter of the operator running on this thread, or nullptr outside any operator scope
	static shared_ptr<OperatorMemoryCounter> CurrentOwner();
};

//! Whether a reservation obtained by evicting blocks is charged to the operator running on this thread
enum class ReservationAttribution : uint8_t { CURRENT_OPERATOR, NONE };

struct TempBufferPoolReservation : BufferPoolReservation {
	TempBufferPoolReservation(MemoryTag tag, BufferPool &pool, idx_t size) : BufferPoolReservation(tag, pool) {
		Resize(size);
	}
	TempBufferPoolReservation(MemoryTag tag, BufferPool &pool, idx_t size, shared_ptr<OperatorMemoryCounter> owner)
	    : BufferPoolReservation(tag, pool, std::move(owner)) {
		Resize(size);
	}
	TempBufferPoolReservation(TempBufferPoolReservation &&) = default;
	~TempBufferPoolReservation() override {
		Resize(0);
	}
};

} // namespace duckdb
