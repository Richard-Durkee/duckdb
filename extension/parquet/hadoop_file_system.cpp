#include "hadoop_file_system.hpp"

#include "lz4.hpp"
#include "snappy.h"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

namespace {

constexpr idx_t HADOOP_IN_BUFFER_SIZE = 1 << 20;
constexpr idx_t HADOOP_OUT_BUFFER_SIZE = 1 << 20;
//! A block or chunk length above this is a corrupt file, not something to allocate
constexpr idx_t HADOOP_MAX_BLOCK_SIZE = 1 << 28;
//! The first four bytes of an lz4 frame format file, and of a snappy framing format file (its stream identifier)
constexpr data_t LZ4_FRAME_MAGIC[] = {0x04, 0x22, 0x4D, 0x18};
constexpr data_t SNAPPY_FRAMING_MAGIC[] = {0xFF, 0x06, 0x00, 0x00};

const char *CodecName(HadoopBlockCodec codec) {
	return codec == HadoopBlockCodec::LZ4 ? "lz4" : "snappy";
}

uint32_t LoadBigEndian(const_data_ptr_t bytes) {
	return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

//! Room for 'size' more bytes after out_buff_end. CompressedFile drains the output buffer before every Read and keeps
//! no other pointer into it, so it can be replaced by a larger one.
void ReserveOutput(StreamData &sd, idx_t size) {
	auto used = NumericCast<idx_t>(sd.out_buff_end - sd.out_buff.get());
	if (used + size <= sd.out_buf_size) {
		return;
	}
	auto start = NumericCast<idx_t>(sd.out_buff_start - sd.out_buff.get());
	auto new_size = MaxValue<idx_t>(sd.out_buf_size * 2, used + size);
	auto new_buff = make_unsafe_uniq_array<data_t>(new_size);
	memcpy(new_buff.get(), sd.out_buff.get(), used);
	sd.out_buff = std::move(new_buff);
	sd.out_buf_size = new_size;
	sd.out_buff_start = sd.out_buff.get() + start;
	sd.out_buff_end = sd.out_buff.get() + used;
}

class HadoopBlockStreamWrapper : public StreamWrapper {
public:
	explicit HadoopBlockStreamWrapper(HadoopBlockCodec codec_p) : codec(codec_p) {
	}

	void Initialize(QueryContext context, CompressedFile &file, bool write) override {
		D_ASSERT(!write);
		path = file.path;
	}

	bool Read(StreamData &sd) override {
		// at most one chunk per call, decoded into the (drained) output buffer, so no decoded data is held back:
		// CompressedFile does not call Read again once the input is exhausted
		const_data_ptr_t bytes;
		while (true) {
			switch (state) {
			case State::BLOCK_LENGTH:
				if (!Take(sd, sizeof(uint32_t), bytes)) {
					return false;
				}
				if (at_start) {
					at_start = false;
					CheckNotFramed(bytes);
				}
				block_remaining = CheckedLength(LoadBigEndian(bytes));
				if (block_remaining > 0) {
					state = State::CHUNK_LENGTH;
				}
				break;
			case State::CHUNK_LENGTH:
				if (!Take(sd, sizeof(uint32_t), bytes)) {
					return false;
				}
				chunk_length = CheckedLength(LoadBigEndian(bytes));
				state = State::CHUNK;
				break;
			case State::CHUNK:
				if (!Take(sd, chunk_length, bytes)) {
					return false;
				}
				DecodeChunk(sd, bytes);
				state = block_remaining > 0 ? State::CHUNK_LENGTH : State::BLOCK_LENGTH;
				return false;
			}
		}
	}

	void FinalizeRead(StreamData &sd) override {
		if (state != State::BLOCK_LENGTH || !pending.empty()) {
			throw IOException("Unexpected end of %s stream of \"%s\"", CodecName(codec), path);
		}
	}

	void Write(CompressedFile &file, StreamData &sd, data_ptr_t buffer, int64_t nr_bytes) override {
		throw InternalException("Hadoop %s stream opened for reading was written to", CodecName(codec));
	}

	void Close() override {
	}

private:
	enum class State : uint8_t { BLOCK_LENGTH, CHUNK_LENGTH, CHUNK };

	//! The snappy framing and lz4 frame formats are different containers around the same codecs: fail clearly rather
	//! than reading their headers as block lengths
	void CheckNotFramed(const_data_ptr_t bytes) const {
		if (codec == HadoopBlockCodec::LZ4 && memcmp(bytes, LZ4_FRAME_MAGIC, sizeof(LZ4_FRAME_MAGIC)) == 0) {
			throw NotImplementedException("\"%s\" is in the lz4 frame format: only lz4 files as Hadoop's Lz4Codec "
			                              "writes them are supported",
			                              path);
		}
		if (codec == HadoopBlockCodec::SNAPPY &&
		    memcmp(bytes, SNAPPY_FRAMING_MAGIC, sizeof(SNAPPY_FRAMING_MAGIC)) == 0) {
			throw NotImplementedException("\"%s\" is in the snappy framing format: only snappy files as Hadoop's "
			                              "SnappyCodec writes them are supported",
			                              path);
		}
	}

	idx_t CheckedLength(uint32_t length) const {
		if (length > HADOOP_MAX_BLOCK_SIZE) {
			throw IOException("Corrupt %s stream of \"%s\": block length %d", CodecName(codec), path, length);
		}
		return length;
	}

	//! Point 'result' at the next 'count' bytes of the stream and consume them. Bytes that span input buffers are
	//! gathered in 'pending' (false is returned until all have arrived); 'result' stays valid until the next Take.
	bool Take(StreamData &sd, idx_t count, const_data_ptr_t &result) {
		auto available = NumericCast<idx_t>(sd.in_buff_end - sd.in_buff_start);
		if (pending.empty() && available >= count) {
			result = sd.in_buff_start;
			sd.in_buff_start += count;
			return true;
		}
		auto copy = MinValue<idx_t>(count - pending.size(), available);
		pending.insert(pending.end(), sd.in_buff_start, sd.in_buff_start + copy);
		sd.in_buff_start += copy;
		if (pending.size() < count) {
			return false;
		}
		taken.swap(pending);
		pending.clear();
		result = taken.data();
		return true;
	}

	void DecodeChunk(StreamData &sd, const_data_ptr_t chunk) {
		idx_t decoded;
		if (codec == HadoopBlockCodec::LZ4) {
			ReserveOutput(sd, block_remaining);
			auto result =
			    duckdb_lz4::LZ4_decompress_safe(const_char_ptr_cast(chunk), char_ptr_cast(sd.out_buff_end),
			                                    NumericCast<int>(chunk_length), NumericCast<int>(block_remaining));
			if (result < 0) {
				throw IOException("Corrupt lz4 stream of \"%s\": a block does not decompress", path);
			}
			decoded = NumericCast<idx_t>(result);
		} else {
			size_t length;
			if (!duckdb_snappy::GetUncompressedLength(const_char_ptr_cast(chunk), chunk_length, &length) ||
			    length > block_remaining) {
				throw IOException("Corrupt snappy stream of \"%s\": a block has an invalid length", path);
			}
			ReserveOutput(sd, length);
			if (!duckdb_snappy::RawUncompress(const_char_ptr_cast(chunk), chunk_length,
			                                  char_ptr_cast(sd.out_buff_end))) {
				throw IOException("Corrupt snappy stream of \"%s\": a block does not decompress", path);
			}
			decoded = length;
		}
		sd.out_buff_end += decoded;
		block_remaining -= decoded;
	}

private:
	HadoopBlockCodec codec;
	string path;
	State state = State::BLOCK_LENGTH;
	bool at_start = true;
	idx_t block_remaining = 0;
	idx_t chunk_length = 0;
	vector<data_t> pending;
	vector<data_t> taken;
};

//! A file handle owns the file system it decompresses with, like ZStdFile
struct HadoopFileSystemHolder {
	explicit HadoopFileSystemHolder(HadoopBlockCodec codec) : hadoop_fs(codec) {
	}
	HadoopFileSystem hadoop_fs;
};

class HadoopCompressedFile : private HadoopFileSystemHolder, public CompressedFile {
public:
	HadoopCompressedFile(HadoopBlockCodec codec, QueryContext context, unique_ptr<FileHandle> child_handle_p,
	                     const string &path)
	    : HadoopFileSystemHolder(codec), CompressedFile(hadoop_fs, std::move(child_handle_p), path) {
		Initialize(context, false);
	}

	FileCompressionType GetFileCompressionType() override {
		return hadoop_fs.GetCompressionType();
	}
};

} // namespace

HadoopFileSystem::HadoopFileSystem(HadoopBlockCodec codec_p) : codec(codec_p) {
}

unique_ptr<FileHandle> HadoopFileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                            bool write) {
	if (write) {
		throw NotImplementedException("Writing %s compressed files is not supported (\"%s\")", CodecName(codec),
		                              handle->path);
	}
	auto path = handle->path;
	return make_uniq<HadoopCompressedFile>(codec, context, std::move(handle), path);
}

std::string HadoopFileSystem::GetName() const {
	return codec == HadoopBlockCodec::LZ4 ? "HadoopLZ4FileSystem" : "HadoopSnappyFileSystem";
}

FileCompressionType HadoopFileSystem::GetCompressionType() {
	return FileCompressionType(CodecName(codec));
}

bool HadoopFileSystem::CanHandleFile(const string &fpath) {
	// the extensions Hadoop's CompressionCodecFactory picks these codecs by
	auto end = fpath.find('?');
	auto name = StringUtil::Lower(end == string::npos ? fpath : fpath.substr(0, end));
	return StringUtil::EndsWith(name, codec == HadoopBlockCodec::LZ4 ? ".lz4" : ".snappy");
}

unique_ptr<StreamWrapper> HadoopFileSystem::CreateStream() {
	return make_uniq<HadoopBlockStreamWrapper>(codec);
}

idx_t HadoopFileSystem::InBufferSize() {
	return HADOOP_IN_BUFFER_SIZE;
}

idx_t HadoopFileSystem::OutBufferSize() {
	return HADOOP_OUT_BUFFER_SIZE;
}

} // namespace duckdb
