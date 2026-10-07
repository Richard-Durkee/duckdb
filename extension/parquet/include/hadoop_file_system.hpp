//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hadoop_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/compressed_file_system.hpp"

namespace duckdb {

enum class HadoopBlockCodec : uint8_t { SNAPPY, LZ4 };

//! Reads the snappy and lz4 files Hadoop's SnappyCodec and Lz4Codec write, which is how Hive, Spark and Trino
//! compress text files: blocks of the big-endian uncompressed length, each followed by chunks of a big-endian
//! compressed length and a raw snappy / lz4 block. These are not the snappy framing or lz4 frame formats.
class HadoopFileSystem : public CompressedFileSystem {
public:
	explicit HadoopFileSystem(HadoopBlockCodec codec);

	unique_ptr<FileHandle> OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle, bool write) override;

	std::string GetName() const override;
	FileCompressionType GetCompressionType() override;
	bool CanHandleFile(const string &fpath) override;

	unique_ptr<StreamWrapper> CreateStream() override;
	idx_t InBufferSize() override;
	idx_t OutBufferSize() override;

private:
	HadoopBlockCodec codec;
};

} // namespace duckdb
