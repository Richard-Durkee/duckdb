#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {
namespace capiv2 {

//! A block of memory a database accounts for: counted against its memory limit, under the EXTENSION tag, until freed.
//! The bytes come from the default allocator rather than the database's, so the block can be freed after the database
//! is gone; the database is held weakly, and the accounting is skipped once it has been destroyed.
class CV2MemoryBuffer {
public:
	CV2MemoryBuffer(DatabaseInstance &db_p, idx_t size_p) : db(db_p.shared_from_this()) {
		CheckSize(size_p);
		if (size_p == 0) {
			return;
		}
		auto &buffer_manager = BufferManager::GetBufferManager(db_p);
		// evicts buffer-managed data to make room, or throws an OutOfMemoryException without counting anything
		buffer_manager.ReserveMemory(size_p);
		try {
			data = Allocator::DefaultAllocator().AllocateData(size_p);
		} catch (...) {
			buffer_manager.FreeReservedMemory(size_p);
			throw;
		}
		size = size_p;
	}

	~CV2MemoryBuffer() {
		if (!data) {
			return;
		}
		Allocator::DefaultAllocator().FreeData(data, size);
		if (auto database = db.lock()) {
			BufferManager::GetBufferManager(*database).FreeReservedMemory(size);
		}
	}

	CV2MemoryBuffer(const CV2MemoryBuffer &) = delete;
	CV2MemoryBuffer &operator=(const CV2MemoryBuffer &) = delete;

	void Resize(idx_t new_size) {
		CheckSize(new_size);
		if (new_size == size) {
			return;
		}
		auto database = db.lock();
		if (!database) {
			throw InvalidInputException("duckdb_v2_memory_buffer_resize: the database of the buffer was closed");
		}
		auto &buffer_manager = BufferManager::GetBufferManager(*database);
		auto &allocator = Allocator::DefaultAllocator();
		if (new_size > size) {
			// account for the additional bytes first: a failure leaves the buffer as it was
			auto additional = new_size - size;
			buffer_manager.ReserveMemory(additional);
			try {
				data = data ? allocator.ReallocateData(data, size, new_size) : allocator.AllocateData(new_size);
			} catch (...) {
				buffer_manager.FreeReservedMemory(additional);
				throw;
			}
		} else if (new_size == 0) {
			allocator.FreeData(data, size);
			data = nullptr;
			buffer_manager.FreeReservedMemory(size);
		} else {
			data = allocator.ReallocateData(data, size, new_size);
			buffer_manager.FreeReservedMemory(size - new_size);
		}
		size = new_size;
	}

	data_ptr_t data = nullptr;
	idx_t size = 0;

private:
	//! The largest allocation the allocator serves, which it treats as an internal error rather than as input to reject
	static constexpr idx_t MAXIMUM_SIZE = 281474976710656ULL;

	static void CheckSize(idx_t size) {
		if (size >= MAXIMUM_SIZE) {
			throw OutOfRangeException("Requested allocation size of %llu is out of range - maximum allocation size is "
			                          "%llu",
			                          size, MAXIMUM_SIZE);
		}
	}

	weak_ptr<DatabaseInstance> db;
};

static auto Convert(duckdb_v2_memory_buffer_handle buffer) -> CV2MemoryBuffer * {
	return reinterpret_cast<CV2MemoryBuffer *>(buffer);
}

static auto Convert(CV2MemoryBuffer *buffer) -> duckdb_v2_memory_buffer_handle {
	return reinterpret_cast<duckdb_v2_memory_buffer_handle>(buffer);
}

} // namespace capiv2
} // namespace duckdb

//----------------------------------------------------------------------------------------------------------------------
// Public API
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_context_allocate_memory(duckdb_v2_context_handle context, idx_t size,
                                                  duckdb_v2_memory_buffer_handle *out_buffer,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_buffer);
	*out_buffer = nullptr;
	DUCKDB_CHECK_ARG(context);
	return WithErrorHandler(err, [&]() {
		auto &db = duckdb::DatabaseInstance::GetDatabase(Convert(context)->context);
		*out_buffer = Convert(new CV2MemoryBuffer(db, size));
	});
}

DUCKDB_V2_ERROR duckdb_v2_instance_allocate_memory(duckdb_v2_instance_handle instance, idx_t size,
                                                   duckdb_v2_memory_buffer_handle *out_buffer,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_buffer);
	*out_buffer = nullptr;
	DUCKDB_CHECK_ARG(instance);
	return WithErrorHandler(err, [&]() {
		auto &db = *Convert(instance)->GetDatabase().instance;
		*out_buffer = Convert(new CV2MemoryBuffer(db, size));
	});
}

DUCKDB_V2_ERROR duckdb_v2_memory_buffer_get_data(duckdb_v2_memory_buffer_handle buffer, uint8_t **out_ptr,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_ptr);
	*out_ptr = nullptr;
	DUCKDB_CHECK_ARG(buffer);
	return WithErrorHandler(err, [&]() { *out_ptr = Convert(buffer)->data; });
}

DUCKDB_V2_ERROR duckdb_v2_memory_buffer_get_size(duckdb_v2_memory_buffer_handle buffer, idx_t *size,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(size);
	DUCKDB_CHECK_ARG(buffer);
	return WithErrorHandler(err, [&]() { *size = Convert(buffer)->size; });
}

DUCKDB_V2_ERROR duckdb_v2_memory_buffer_resize(duckdb_v2_memory_buffer_handle buffer, idx_t size,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(buffer);
	return WithErrorHandler(err, [&]() { Convert(buffer)->Resize(size); });
}

DUCKDB_V2_ERROR duckdb_v2_memory_buffer_destroy(duckdb_v2_memory_buffer_handle *buffer) {
	return WithErrorHandler(nullptr, [&]() {
		if (!buffer) {
			return;
		}
		if (*buffer) {
			delete Convert(*buffer);
			*buffer = nullptr;
		}
	});
}
