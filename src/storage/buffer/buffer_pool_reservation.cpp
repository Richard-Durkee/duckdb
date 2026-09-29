#include "duckdb/storage/buffer/buffer_pool_reservation.hpp"

#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer/memory_account.hpp"

namespace duckdb {

BufferPoolReservation::BufferPoolReservation(MemoryTag tag, BufferPool &pool) : tag(tag), pool(pool) {
}

BufferPoolReservation::BufferPoolReservation(BufferPoolReservation &&src) noexcept : tag(src.tag), pool(src.pool) {
	size = src.size;
	account = std::move(src.account);
	src.size = 0;
}

BufferPoolReservation &BufferPoolReservation::operator=(BufferPoolReservation &&src) noexcept {
	pool.UpdateUsedMemory(tag, -UnsafeNumericCast<int64_t>(size));
	if (account) {
		account->Update(-UnsafeNumericCast<int64_t>(size));
	}
	tag = src.tag;
	size = src.size;
	account = std::move(src.account);
	src.size = 0;
	return *this;
}

BufferPoolReservation::~BufferPoolReservation() {
	D_ASSERT(size == 0);
}

void BufferPoolReservation::Resize(idx_t new_size) {
	auto delta = UnsafeNumericCast<int64_t>(new_size) - UnsafeNumericCast<int64_t>(size);
	pool.UpdateUsedMemory(tag, delta);
	if (account) {
		account->Update(delta);
	}
	size = new_size;
}

void BufferPoolReservation::Merge(BufferPoolReservation src) {
	// src's bytes now belong to this reservation's account
	if (src.account != account) {
		src.SetAccount(account);
	}
	size += src.size;
	src.size = 0;
}

void BufferPoolReservation::SetAccount(shared_ptr<MemoryAccount> new_account) {
	if (new_account == account) {
		return;
	}
	auto bytes = UnsafeNumericCast<int64_t>(size);
	if (account) {
		account->Update(-bytes);
	}
	if (new_account) {
		new_account->Update(bytes);
	}
	account = std::move(new_account);
}

} // namespace duckdb
