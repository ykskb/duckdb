#pragma once
#include <iostream>
#include <list>
#include "thrift/protocol/TCompactProtocol.h"
#include "thrift/transport/TBufferTransports.h"
#include "duckdb/common/thread.hpp"

#include "duckdb.hpp"
#ifndef DUCKDB_AMALGAMATION
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/likely.hpp"
#endif

namespace duckdb {

/// A ReadHead for prefetching data in a specific range
struct ReadHead {
	ReadHead(idx_t location, size_t size) : location(location), size(size) {};
	/// Hint info
	idx_t location;
	size_t size;

	/// Current info
	unique_ptr<AllocatedData> data;

	idx_t GetEnd() const {
		return size + location;
	}

	void Allocate(Allocator &allocator) {
		data = allocator.Allocate(size);
	}
};

// Comparator for ReadHeads that are either overlapping, adjacent, or within ALLOW_GAP bytes from each other
struct ReadHeadComparator {
	static constexpr size_t ALLOW_GAP = 1 << 14; // 16 KiB
	bool operator()(const ReadHead *a, const ReadHead *b) const {
		auto a_start = a->location;
		auto a_end = a->location + a->size;
		auto b_start = b->location;

		if (DUCKDB_LIKELY(a_end <= NumericLimits<idx_t>::Maximum() - ALLOW_GAP)) {
			a_end += ALLOW_GAP;
		}

		return a_start < b_start && a_end < b_start;
	}
};

/// Two-step read ahead buffer
/// 1: register all ranges that will be read, merging ranges that are consecutive
/// 2: prefetch all registered ranges
struct ReadAheadBuffer {
	ReadAheadBuffer(Allocator &allocator, FileHandle &handle, FileOpener &opener)
	    : allocator(allocator), handle(handle), file_opener(opener) {
	}

	/// The list of read heads
	std::list<ReadHead> read_heads;
	/// Store copies of file handles for efficient async prefetching.
	vector<unique_ptr<FileHandle>> handle_copies;
	/// Set for merging consecutive ranges
	std::set<ReadHead *, ReadHeadComparator> merge_set;

	Allocator &allocator;
	FileHandle &handle;
	FileOpener &file_opener;

	idx_t total_size = 0;

	/// Add a read head to the prefetching list
	void AddReadHead(idx_t pos, idx_t len, bool merge_buffers = true) {
		// Attempt to merge with existing
		if (merge_buffers) {
			ReadHead new_read_head {pos, len};
			auto lookup_set = merge_set.find(&new_read_head);
			if (lookup_set != merge_set.end()) {
				auto existing_head = *lookup_set;
				auto new_start = MinValue<idx_t>(existing_head->location, new_read_head.location);
				auto new_length = MaxValue<idx_t>(existing_head->GetEnd(), new_read_head.GetEnd()) - new_start;
				existing_head->location = new_start;
				existing_head->size = new_length;
				return;
			}
		}

		read_heads.emplace_front(ReadHead(pos, len));
		total_size += len;
		auto &read_head = read_heads.front();
		merge_set.insert(&read_head);

		if (read_head.GetEnd() > handle.GetFileSize()) {
			throw std::runtime_error("Prefetch registered for bytes outside file");
		}
	}

	/// Returns the relevant read head
	ReadHead *GetReadHead(idx_t pos) {
		for (auto &read_head : read_heads) {
			if (pos >= read_head.location && pos < read_head.GetEnd()) {
				return &read_head;
			}
		}
		return nullptr;
	}

	/// Prefetch all read heads
	void Prefetch() {
		std::vector<thread> download_threads;
		std::vector<unique_ptr<FileHandle>> handles_in_progress;

		bool async = read_heads.size() >= 2;

		for (auto &read_head : read_heads) {
			read_head.Allocate(allocator);

			if (read_head.GetEnd() > handle.GetFileSize()) {
				throw std::runtime_error("Prefetch registered requested for bytes outside file");
			}

			if (async) {
				unique_ptr<FileHandle> file_handle;
				if (handle_copies.size() > 0) {
					file_handle = std::move(handle_copies.back());
					handle_copies.pop_back();
				} else {
					auto flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_DIRECT_IO;
					file_handle = handle.file_system.OpenFile(handle.path, flags, FileSystem::DEFAULT_LOCK,
					                                          FileSystem::DEFAULT_COMPRESSION, &file_opener);
				}
				auto file_handle_ptr = file_handle.get();
				handles_in_progress.push_back(std::move(file_handle));

				// Start download thread
				thread upload_thread(
				    [](ReadHead &read_head, FileHandle *file_handle) {
					    file_handle->Read(read_head.data->get(), read_head.size, read_head.location);
					    //					    std::cout << "Prefetch async new " << read_head.location << " for " <<
					    //read_head.size
					    //					              << " bytes\n";
				    },
				    std::ref(read_head), file_handle_ptr);

				download_threads.push_back(std::move(upload_thread));
			} else {
				handle.Read(read_head.data->get(), read_head.size, read_head.location);
				//				std::cout << "Prefetch sync new " << read_head.location << " for " << read_head.size
				//				          << " bytes\n";
			}
		}

		if (async) {
			// Await all outstanding requests
			for (auto &thread : download_threads) {
				thread.join();
			}

			// Store handles for reuse
			for (auto &handle : handles_in_progress) {
				handle_copies.push_back(std::move(handle));
			}
		}

		// Delete the merge set to prevent any further any further merges on the already prefetched buffers.
		merge_set.clear();
	}
};

class ThriftFileTransport : public duckdb_apache::thrift::transport::TVirtualTransport<ThriftFileTransport> {
public:
	static constexpr size_t PREFETCH_FALLBACK_BUFFERSIZE = 1000000;

	ThriftFileTransport(Allocator &allocator, FileHandle &handle_p, FileOpener &opener, bool prefetch_mode_p)
	    : allocator(allocator), handle(handle_p), location(0), ra_buffer(ReadAheadBuffer(allocator, handle_p, opener)),
	      prefetch_mode(prefetch_mode_p) {
	}

	uint32_t read(uint8_t *buf, uint32_t len) {
		auto prefetch_buffer = ra_buffer.GetReadHead(location);
		if (prefetch_buffer != nullptr && location - prefetch_buffer->location + len <= prefetch_buffer->size) {
			D_ASSERT(location - prefetch_buffer->location + len <= prefetch_buffer->size);
			memcpy(buf, prefetch_buffer->data->get() + location - prefetch_buffer->location, len);
		} else {
			if (prefetch_mode && len < PREFETCH_FALLBACK_BUFFERSIZE && len > 0) {
				// We've reached a non-prefetched address in prefetch_mode, this should normally not happen,
				// but just in case we fall back to buffered reads. The assertion will trigger in tests in debug mode to
				// confirm the prefetching works
				Prefetch(location, MinValue<size_t>(PREFETCH_FALLBACK_BUFFERSIZE, handle.GetFileSize() - location));
				auto prefetch_buffer_fallback = ra_buffer.GetReadHead(location);
				D_ASSERT(location - prefetch_buffer_fallback->location + len <= prefetch_buffer_fallback->size);
				memcpy(buf, prefetch_buffer_fallback->data->get() + location - prefetch_buffer_fallback->location, len);
			} else {
				handle.Read(buf, len, location);
			}
		}
		location += len;
		return len;
	}

	void Prefetch(idx_t pos, idx_t len) {
		RegisterPrefetch(pos, len);
		PrefetchRegistered();
	}

	/// New prefetch
	void RegisterPrefetch(idx_t pos, idx_t len) {
		ra_buffer.AddReadHead(pos, len);
	}

	void PrefetchRegistered() {
		ra_buffer.Prefetch();
	}
	void ClearPrefetch() {
		ra_buffer.read_heads.clear();
	}

	void SetLocation(idx_t location_p) {
		location = location_p;
	}

	idx_t GetLocation() {
		return location;
	}
	idx_t GetSize() {
		return handle.file_system.GetFileSize(handle);
	}

private:
	Allocator &allocator;
	FileHandle &handle;
	idx_t location;

	// Multi-buffer prefetch
	ReadAheadBuffer ra_buffer;

	/// Whether the prefetch mode is enabled. In this mode the DirectIO flag of the handle will be set and the parquet
	/// reader will manage the read buffering.
	bool prefetch_mode;
};

} // namespace duckdb
