#include "duckdb/execution/operator/order/physical_order.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_context.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"
#include "duckdb/storage/statistics/string_statistics.hpp"

namespace duckdb {

PhysicalOrder::PhysicalOrder(vector<LogicalType> types, vector<BoundOrderByNode> orders, idx_t estimated_cardinality)
    : PhysicalSink(PhysicalOperatorType::ORDER_BY, move(types), estimated_cardinality), orders(move(orders)) {
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
struct SortingState {
	const idx_t ENTRY_SIZE;

	const vector<OrderType> ORDER_TYPES;
	const vector<OrderByNullType> ORDER_BY_NULL_TYPES;
	const vector<LogicalType> TYPES;
	const vector<BaseStatistics *> STATS;

	const vector<bool> HAS_NULL;
	const vector<bool> CONSTANT_SIZE;
	const vector<idx_t> COL_SIZE;

	const vector<idx_t> ROWCHUNK_INIT_SIZES;
};

struct PayloadState {
	const bool HAS_VARIABLE_SIZE;
	const idx_t VALIDITYMASK_SIZE;
	const idx_t ENTRY_SIZE;

	const idx_t ROWCHUNK_INIT_SIZE;
};

class OrderGlobalState : public GlobalOperatorState {
public:
	explicit OrderGlobalState(BufferManager &buffer_manager) : buffer_manager(buffer_manager) {
	}
	//! The lock for updating the order global state
	std::mutex lock;
	//! The buffer manager
	BufferManager &buffer_manager;

	//! Constants concerning sorting and/or payload data
	unique_ptr<SortingState> sorting_state;
	unique_ptr<PayloadState> payload_state;

	//! Sorted data
	vector<unique_ptr<ContinuousBlock>> sorted_blocks;

	//! Total count - set after PhysicalOrder::Finalize is called
	idx_t total_count;
};

class OrderLocalState : public LocalSinkState {
public:
	OrderLocalState() : initialized(false) {
	}

	//! Whether this local state has been initialized
	bool initialized;

	//! Local copy of the executor
	ExpressionExecutor executor;

	//! Holds a vector of incoming sorting columns
	DataChunk sort;

	void Initialize(BufferManager &buffer_manager, const SortingState &sorting_state,
	                const PayloadState &payload_state) {
		// sorting block
		idx_t vectors_per_block =
		    (Storage::BLOCK_ALLOC_SIZE / sorting_state.ENTRY_SIZE + STANDARD_VECTOR_SIZE) / STANDARD_VECTOR_SIZE;
		sorting_block =
		    make_unique<RowChunk>(buffer_manager, vectors_per_block * STANDARD_VECTOR_SIZE, sorting_state.ENTRY_SIZE);
		// variable sorting column blocks
		for (idx_t i = 0; i < sorting_state.CONSTANT_SIZE.size(); i++) {
			if (sorting_state.CONSTANT_SIZE[i]) {
				var_sorting_blocks.push_back(nullptr);
				var_sorting_sizes.push_back(nullptr);
			} else {
				var_sorting_blocks.push_back(
				    make_unique<RowChunk>(buffer_manager, sorting_state.ROWCHUNK_INIT_SIZES[i] / 8, 8));
				var_sorting_sizes.push_back(make_unique<RowChunk>(
				    buffer_manager, (idx_t)Storage::BLOCK_ALLOC_SIZE / sizeof(idx_t) + 1, sizeof(idx_t)));
			}
		}
		// payload block
		if (payload_state.HAS_VARIABLE_SIZE) {
			payload_block = make_unique<RowChunk>(buffer_manager, payload_state.ROWCHUNK_INIT_SIZE / 32, 32);
			sizes_block = make_unique<RowChunk>(buffer_manager, (idx_t)Storage::BLOCK_ALLOC_SIZE / sizeof(idx_t) + 1,
			                                    sizeof(idx_t));
		} else {
			payload_block = make_unique<RowChunk>(
			    buffer_manager, payload_state.ROWCHUNK_INIT_SIZE / payload_state.ENTRY_SIZE, payload_state.ENTRY_SIZE);
		}
		initialized = true;
	}

	//! Sorting columns, and variable size sorting data (if any)
	unique_ptr<RowChunk> sorting_block = nullptr;
	vector<unique_ptr<RowChunk>> var_sorting_blocks;
	vector<unique_ptr<RowChunk>> var_sorting_sizes;

	//! Payload data (and payload entry sizes if there is variable size data)
	unique_ptr<RowChunk> payload_block = nullptr;
	unique_ptr<RowChunk> sizes_block = nullptr;

	//! Sorted data
	vector<unique_ptr<ContinuousBlock>> sorted_blocks;

	//! Constant buffers allocated for vector serialization
	const SelectionVector *sel_ptr = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
	data_ptr_t key_locations[STANDARD_VECTOR_SIZE];
	data_ptr_t validitymask_locations[STANDARD_VECTOR_SIZE];
	idx_t entry_sizes[STANDARD_VECTOR_SIZE];
};

unique_ptr<GlobalOperatorState> PhysicalOrder::GetGlobalState(ClientContext &context) {
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	auto state = make_unique<OrderGlobalState>(buffer_manager);

	// init sorting state and sorting block
	size_t entry_size = 0;
	vector<OrderType> order_types;
	vector<OrderByNullType> order_by_null_types;
	vector<LogicalType> types;
	vector<BaseStatistics *> stats;
	vector<bool> has_null;
	vector<bool> constant_size;
	vector<idx_t> col_sizes;
	vector<idx_t> rowchunk_init_sizes;
	for (auto &order : orders) {
		// global state ExpressionExecutor
		auto &expr = *order.expression;

		// sorting state
		order_types.push_back(order.type);
		order_by_null_types.push_back(order.null_order);
		types.push_back(expr.return_type);
		if (expr.stats) {
			stats.push_back(expr.stats.get());
		} else {
			stats.push_back(nullptr);
		}

		// compute column sizes
		auto physical_type = expr.return_type.InternalType();
		constant_size.push_back(TypeIsConstantSize(physical_type));
		idx_t col_size = GetTypeIdSize(expr.return_type.InternalType());

		// TODO: make use of statistics
		if (!TypeIsConstantSize(physical_type)) {
			switch (physical_type) {
			case PhysicalType::VARCHAR:
				col_size = StringStatistics::MAX_STRING_MINMAX_SIZE;
				break;
			default:
				// do nothing
				break;
			}
		}
		has_null.push_back(true);

		// increment entry size with the column size
		if (has_null.back()) {
			col_size++;
		}
		entry_size += col_size;
		col_sizes.push_back(col_size);

		// create RowChunks for variable size sorting columns in order to resolve
		if (TypeIsConstantSize(physical_type)) {
			rowchunk_init_sizes.push_back(0);
		} else {
			// besides the prefix, variable size sorting columns are also fully serialized, along with offsets
			// we have to assume a large variable size, otherwise a single large variable entry may not fit in a block
			// 1 << 23 = 8MB
			rowchunk_init_sizes.push_back((1 << 23) / 8);
		}
	}
	// make room for an 'index' column at the end
	entry_size += sizeof(idx_t);
	state->sorting_state =
	    unique_ptr<SortingState>(new SortingState {entry_size, order_types, order_by_null_types, types, stats, has_null,
	                                               constant_size, col_sizes, rowchunk_init_sizes});

	// init payload state
	entry_size = 0;
	idx_t validitymask_size = (children[0]->types.size() + 7) / 8;
	entry_size += validitymask_size;
	bool variable_payload_size = false;
	idx_t var_columns = 0;
	for (auto &type : children[0]->types) {
		auto physical_type = type.InternalType();
		if (TypeIsConstantSize(physical_type)) {
			entry_size += GetTypeIdSize(physical_type);
		} else {
			variable_payload_size = true;
			var_columns++;
		}
	}
	idx_t rowchunk_init_size;
	if (variable_payload_size) {
		rowchunk_init_size = entry_size + var_columns * (1 << 23);
	} else {
		idx_t vectors_per_block =
		    (Storage::BLOCK_ALLOC_SIZE / entry_size + STANDARD_VECTOR_SIZE) / STANDARD_VECTOR_SIZE;
		rowchunk_init_size = vectors_per_block * STANDARD_VECTOR_SIZE * entry_size;
	}
	state->payload_state = unique_ptr<PayloadState>(
	    new PayloadState {variable_payload_size, validitymask_size, entry_size, rowchunk_init_size});
	return move(state);
}

unique_ptr<LocalSinkState> PhysicalOrder::GetLocalSinkState(ExecutionContext &context) {
	auto result = make_unique<OrderLocalState>();
	vector<LogicalType> types;
	for (auto &order : orders) {
		types.push_back(order.expression->return_type);
		result->executor.AddExpression(*order.expression);
	}
	result->sort.Initialize(types);
	return move(result);
}

void PhysicalOrder::Sink(ExecutionContext &context, GlobalOperatorState &gstate_p, LocalSinkState &lstate_p,
                         DataChunk &input) {
	auto &gstate = (OrderGlobalState &)gstate_p;
	auto &lstate = (OrderLocalState &)lstate_p;
	const auto &sorting_state = *gstate.sorting_state;
	const auto &payload_state = *gstate.payload_state;

	if (!lstate.initialized) {
		lstate.Initialize(BufferManager::GetBufferManager(context.client), *gstate.sorting_state,
		                  *gstate.payload_state);
	}

	// obtain sorting columns
	auto &sort = lstate.sort;
	lstate.executor.Execute(input, sort);

	// build and serialize sorting data
	lstate.sorting_block->Build(sort.size(), lstate.key_locations, nullptr);
	for (idx_t sort_col = 0; sort_col < sort.ColumnCount(); sort_col++) {
		bool has_null = sorting_state.HAS_NULL[sort_col];
		bool nulls_first = sorting_state.ORDER_BY_NULL_TYPES[sort_col] == OrderByNullType::NULLS_FIRST;
		bool desc = sorting_state.ORDER_TYPES[sort_col] == OrderType::DESCENDING;
		idx_t size_in_bytes = StringStatistics::MAX_STRING_MINMAX_SIZE; // TODO: use actual string statistics
		lstate.sorting_block->SerializeVectorSortable(sort.data[sort_col], sort.size(), *lstate.sel_ptr, sort.size(),
		                                              lstate.key_locations, desc, has_null, nulls_first, size_in_bytes);
	}

	// also fully serialize variable size sorting columns
	for (idx_t sort_col = 0; sort_col < sort.ColumnCount(); sort_col++) {
		if (TypeIsConstantSize(sort.data[sort_col].GetType().InternalType())) {
			continue;
		}
		auto &var_sizes = *lstate.var_sorting_sizes[sort_col];
		auto &var_block = *lstate.var_sorting_blocks[sort_col];
		// compute entry sizes
		std::fill_n(lstate.entry_sizes, input.size(), 0);
		RowChunk::ComputeEntrySizes(sort.data[sort_col], lstate.entry_sizes, sort.size());
		// build and serialize entry sizes
		var_sizes.Build(sort.size(), lstate.key_locations, nullptr);
		for (idx_t i = 0; i < input.size(); i++) {
			Store<idx_t>(lstate.entry_sizes[i], lstate.key_locations[i]);
		}
		// build and serialize variable size entries
		var_block.Build(sort.size(), lstate.key_locations, lstate.entry_sizes);
		var_block.SerializeVector(sort.data[sort_col], sort.size(), *lstate.sel_ptr, input.size(), 0,
		                          lstate.key_locations, nullptr);
	}

	// compute entry sizes of payload columns if there are variable size columns
	if (payload_state.HAS_VARIABLE_SIZE) {
		RowChunk::ComputeEntrySizes(input, lstate.entry_sizes, payload_state.ENTRY_SIZE);
		lstate.sizes_block->Build(input.size(), lstate.key_locations, nullptr);
		for (idx_t i = 0; i < input.size(); i++) {
			Store<idx_t>(lstate.entry_sizes[i], lstate.key_locations[i]);
		}
		lstate.payload_block->Build(input.size(), lstate.key_locations, lstate.entry_sizes);
	} else {
		lstate.payload_block->Build(input.size(), lstate.key_locations, nullptr);
	}

	// serialize payload data
	for (idx_t i = 0; i < input.size(); i++) {
		memset(lstate.key_locations[i], -1, payload_state.VALIDITYMASK_SIZE);
		lstate.validitymask_locations[i] = lstate.key_locations[i];
		lstate.key_locations[i] += payload_state.VALIDITYMASK_SIZE;
	}
	for (idx_t payl_col = 0; payl_col < input.ColumnCount(); payl_col++) {
		lstate.payload_block->SerializeVector(input.data[payl_col], input.size(), *lstate.sel_ptr, input.size(),
		                                      payl_col, lstate.key_locations, lstate.validitymask_locations);
	}

	// when sorting data reaches a certain size, we sort it
	if (lstate.sorting_block->count * sorting_state.ENTRY_SIZE > SORTING_BLOCK_SIZE) {
		SortLocalState(context.client, lstate, *gstate.sorting_state, *gstate.payload_state);
	}
}

void PhysicalOrder::Combine(ExecutionContext &context, GlobalOperatorState &gstate_p, LocalSinkState &lstate_p) {
	auto &gstate = (OrderGlobalState &)gstate_p;
	auto &lstate = (OrderLocalState &)lstate_p;

	if (!lstate.sorting_block) {
		return;
	}

	SortLocalState(context.client, lstate, *gstate.sorting_state, *gstate.payload_state);

	lock_guard<mutex> append_lock(gstate.lock);
	for (auto &cb : lstate.sorted_blocks) {
		gstate.sorted_blocks.push_back(move(cb));
	}
}

struct ContinuousChunk {
public:
	ContinuousChunk(BufferManager &buffer_manager, bool constant_size, idx_t entry_size = 0)
	    : CONSTANT_SIZE(constant_size), ENTRY_SIZE(entry_size), buffer_manager(buffer_manager) {
	}

	data_ptr_t DataPtr() {
		if (CONSTANT_SIZE) {
			return data_ptr + data_entry_idx * ENTRY_SIZE;
		} else {
			return data_ptr + offsets[offset_entry_idx];
		}
	}

	idx_t EntrySize() {
		if (CONSTANT_SIZE) {
			return ENTRY_SIZE;
		} else {
			return offsets[offset_entry_idx + 1] - offsets[offset_entry_idx];
		}
	}

	void Initialize() {
		data_block_idx = 0;
		PinDataBlock();
		if (CONSTANT_SIZE) {
			return;
		}
		offset_block_idx = 0;
		PinOffsetBlock();
	}

	void Advance() {
		// advance data
		if (data_entry_idx < data_blocks[data_block_idx].count - 1) {
			data_entry_idx++;
		} else if (data_block_idx < data_blocks.size() - 1) {
			data_block_idx++;
			PinDataBlock();
		}
		// advance offsets (if needed)
		if (CONSTANT_SIZE) {
			return;
		}
		if (offset_entry_idx < offset_blocks[offset_block_idx].count - 1) {
			offset_entry_idx++;
		} else if (offset_entry_idx < offset_blocks.size() - 1) {
			offset_block_idx++;
			PinOffsetBlock();
		}
	}

	void PinDataBlock() {
		data_entry_idx = 0;
		data_handle = buffer_manager.Pin(data_blocks[data_block_idx].block);
		data_ptr = data_handle->node->buffer;
	}

	void PinOffsetBlock() {
		offset_entry_idx = 0;
		offset_handle = buffer_manager.Pin(offset_blocks[offset_block_idx].block);
		offsets = (idx_t *)offset_handle->node->buffer;
	}

	void CopyEntryFrom(ContinuousChunk &source) {
		D_ASSERT(CONSTANT_SIZE == source.CONSTANT_SIZE);
		D_ASSERT(ENTRY_SIZE == source.ENTRY_SIZE);
		auto *last_data_block = &data_blocks[data_block_idx];
		if (CONSTANT_SIZE) {
			if (last_data_block->count == last_data_block->CAPACITY) {
				data_blocks.emplace_back(buffer_manager, last_data_block->CAPACITY, last_data_block->ENTRY_SIZE);
				data_block_idx++;
				PinDataBlock();
				last_data_block = &data_blocks[data_block_idx];
			}
			memcpy(DataPtr(), source.DataPtr(), ENTRY_SIZE);
			last_data_block->count++;
			data_entry_idx++;
		} else {
			const auto entry_size = source.EntrySize();
			if (last_data_block->byte_offset + entry_size > last_data_block->CAPACITY * last_data_block->ENTRY_SIZE) {
				data_blocks.emplace_back(buffer_manager, last_data_block->CAPACITY, last_data_block->ENTRY_SIZE);
				data_block_idx++;
				PinDataBlock();
				last_data_block = &data_blocks[data_block_idx];
			}
			memcpy(DataPtr(), source.DataPtr(), ENTRY_SIZE);
			last_data_block->count++;
			// offset too
			auto *last_offset_block = &offset_blocks[offset_block_idx];
			if (last_offset_block->count == last_offset_block->CAPACITY) {
				offset_blocks.emplace_back(buffer_manager, last_offset_block->CAPACITY, last_offset_block->ENTRY_SIZE);
				offset_block_idx++;
				PinOffsetBlock();
				offsets[0] = 0;
				last_offset_block = &offset_blocks[offset_block_idx];
			}
			offsets[offset_entry_idx + 1] = offsets[offset_entry_idx] + entry_size;
			last_offset_block->count++;
			offset_entry_idx++;
		}
	}

public:
	//! Data and offset blocks
	vector<RowDataBlock> data_blocks;
	vector<RowDataBlock> offset_blocks;

	const bool CONSTANT_SIZE;
	const idx_t ENTRY_SIZE;

private:
	//! Buffer manager and constants
	BufferManager &buffer_manager;

	//! Data
	unique_ptr<BufferHandle> data_handle;
	data_ptr_t data_ptr;
	idx_t data_block_idx;
	idx_t data_entry_idx;

	//! Offsets (if any)
	unique_ptr<BufferHandle> offset_handle;
	idx_t *offsets;
	idx_t offset_block_idx;
	idx_t offset_entry_idx;
};

struct ContinuousBlock {
public:
	ContinuousBlock(BufferManager &buffer_manager, const SortingState &sorting_state)
	    : block_idx(0), buffer_manager(buffer_manager), sorting_state(sorting_state) {
	}

	bool LessThan(ContinuousBlock &other) {
		// TODO: non-constant size columns
		return memcmp(sorting_ptr, other.sorting_ptr, sorting_state.ENTRY_SIZE - sizeof(idx_t)) < 0;
	}

	bool Done() {
		return block_idx >= sorting_blocks.size();
	}

	void PinBlock() {
		entry_idx = 0;
		sorting_handle = buffer_manager.Pin(sorting_blocks[block_idx].block);
		sorting_ptr = sorting_handle->node->buffer;
	}

	void Advance() {
		if (entry_idx < sorting_blocks[block_idx].count - 1) {
			entry_idx++;
			sorting_ptr += sorting_state.ENTRY_SIZE;
			for (idx_t col_idx = 0; col_idx < sorting_state.CONSTANT_SIZE.size(); col_idx++) {
				if (!sorting_state.CONSTANT_SIZE[col_idx]) {
					var_sorting_chunks[col_idx]->Advance();
				}
			}
			payload_chunk->Advance();
		} else if (block_idx < sorting_blocks.size() - 1) {
			block_idx++;
			PinBlock();
		} else if (block_idx < sorting_blocks.size()) {
			// done
			block_idx++;
		}
	}

	void CopyEntryFrom(ContinuousBlock &source) {
		// fixed-size sorting column and entry idx
		memcpy(sorting_ptr, source.sorting_ptr, sorting_state.ENTRY_SIZE - sizeof(idx_t));
		sorting_ptr += sorting_state.ENTRY_SIZE;
		Store<idx_t>(entry_idx, sorting_ptr - sizeof(idx_t));
		// variable size sorting columns and their offsets
		for (idx_t col_idx = 0; col_idx < sorting_state.CONSTANT_SIZE.size(); col_idx++) {
			if (sorting_state.CONSTANT_SIZE[col_idx]) {
				var_sorting_chunks[col_idx]->CopyEntryFrom(*source.var_sorting_chunks[col_idx]);
			}
		}
		// payload columns and their offsets
		payload_chunk->CopyEntryFrom(*source.payload_chunk);
	}

public:
	//! Memcmp-able representation of sorting columns
	vector<RowDataBlock> sorting_blocks;

	//! Variable size sorting columns
	vector<unique_ptr<ContinuousChunk>> var_sorting_chunks;

	//! Payload columns and their offsets
	unique_ptr<ContinuousChunk> payload_chunk;

private:
	idx_t block_idx;
	idx_t entry_idx;

private:
	//! Buffer manager, and sorting state constants
	BufferManager &buffer_manager;
	const SortingState &sorting_state;

	//! Handle and ptr for sorting_blocks
	unique_ptr<BufferHandle> sorting_handle;
	data_ptr_t sorting_ptr;
};

static void ComputeCountAndCapacity(RowChunk &row_chunk, bool variable_entry_size, idx_t &count, idx_t &capacity) {
	const idx_t &entry_size = row_chunk.entry_size;
	count = 0;
	idx_t total_size = 0;
	for (const auto &block : row_chunk.blocks) {
		count += block.count;
		if (variable_entry_size) {
			total_size += block.byte_offset;
		} else {
			total_size += block.count * entry_size;
		}
	}

	if (variable_entry_size) {
		capacity = MaxValue(Storage::BLOCK_ALLOC_SIZE / entry_size, total_size / entry_size + 1);
	} else {
		capacity = MaxValue(Storage::BLOCK_ALLOC_SIZE / entry_size + 1, count);
	}
}

static RowDataBlock ConcatenateBlocks(BufferManager &buffer_manager, RowChunk &row_chunk, bool variable_entry_size) {
	idx_t total_count;
	idx_t capacity;
	ComputeCountAndCapacity(row_chunk, variable_entry_size, total_count, capacity);
	const idx_t &entry_size = row_chunk.entry_size;

	RowDataBlock new_block(buffer_manager, capacity, entry_size);
	new_block.count = total_count;
	auto new_block_handle = buffer_manager.Pin(new_block.block);
	data_ptr_t new_block_ptr = new_block_handle->node->buffer;

	for (auto &block : row_chunk.blocks) {
		auto block_handle = buffer_manager.Pin(block.block);
		if (variable_entry_size) {
			memcpy(new_block_ptr, block_handle->node->buffer, block.byte_offset);
			new_block_ptr += block.byte_offset;
		} else {
			memcpy(new_block_ptr, block_handle->node->buffer, block.count * entry_size);
			new_block_ptr += block.count * entry_size;
		}
		buffer_manager.UnregisterBlock(block.block->BlockId(), true);
	}
	row_chunk.blocks.clear();
	row_chunk.count = 0;
	return new_block;
}

static RowDataBlock SizesToOffsets(BufferManager &buffer_manager, RowChunk &row_chunk) {
	idx_t total_count;
	idx_t capacity;
	ComputeCountAndCapacity(row_chunk, false, total_count, capacity);

	const idx_t &entry_size = row_chunk.entry_size;
	RowDataBlock new_block(buffer_manager, capacity, entry_size);
	new_block.count = total_count;
	auto new_block_handle = buffer_manager.Pin(new_block.block);
	data_ptr_t new_block_ptr = new_block_handle->node->buffer;
	for (auto &block : row_chunk.blocks) {
		auto block_handle = buffer_manager.Pin(block.block);
		memcpy(new_block_ptr, block_handle->node->buffer, block.count * entry_size);
		new_block_ptr += block.count * entry_size;
		buffer_manager.UnregisterBlock(block.block->BlockId(), true);
	}
	row_chunk.blocks.clear();
	row_chunk.count = 0;
	// convert sizes to offsets
	idx_t *offsets = (idx_t *)new_block_handle->node->buffer;
	idx_t prev = offsets[0];
	offsets[0] = 0;
	idx_t curr;
	for (idx_t i = 1; i < total_count; i++) {
		curr = offsets[i];
		offsets[i] = offsets[i - 1] + prev;
		prev = curr;
	}
	offsets[total_count] = offsets[total_count - 1] + prev;
	return new_block;
}

static bool CompareStrings(const data_ptr_t &l, const data_ptr_t &r, const data_ptr_t &var_dataptr,
                           const idx_t offsets[], const int &order, const idx_t &sorting_size) {
	// use indices to find strings in blob
	idx_t left_idx = Load<idx_t>(l + sorting_size);
	idx_t right_idx = Load<idx_t>(r + sorting_size);
	data_ptr_t left_ptr = var_dataptr + offsets[left_idx];
	data_ptr_t right_ptr = var_dataptr + offsets[right_idx];
	// read string lengths
	uint32_t left_size = Load<uint32_t>(left_ptr);
	uint32_t right_size = Load<uint32_t>(right_ptr);
	left_ptr += string_t::PREFIX_LENGTH;
	right_ptr += string_t::PREFIX_LENGTH;
	// construct strings
	string_t left_val((const char *)left_ptr, left_size);
	string_t right_val((const char *)right_ptr, right_size);

	int comp_res = 1;
	if (Equals::Operation<string_t>(left_val, right_val)) {
		comp_res = 0;
	}
	if (LessThan::Operation<string_t>(left_val, right_val)) {
		comp_res = -1;
	}
	return order * comp_res < 0;
}

static void BreakStringTies(BufferManager &buffer_manager, const data_ptr_t dataptr, const idx_t &start,
                            const idx_t &end, const idx_t &tie_col, bool ties[], const data_ptr_t var_dataptr,
                            const data_ptr_t offsets_ptr, const SortingState &sorting_state) {
	idx_t tie_col_offset = 0;
	for (idx_t i = 0; i < tie_col; i++) {
		tie_col_offset += sorting_state.COL_SIZE[i];
	}
	if (sorting_state.HAS_NULL[tie_col]) {
		char *validity = (char *)dataptr + start * sorting_state.ENTRY_SIZE + tie_col_offset;
		if (sorting_state.ORDER_BY_NULL_TYPES[tie_col] == OrderByNullType::NULLS_FIRST && *validity == 0) {
			// NULLS_FIRST, therefore null is encoded as 0 - we can't break null ties
			return;
		} else if (sorting_state.ORDER_BY_NULL_TYPES[tie_col] == OrderByNullType::NULLS_LAST && *validity == 1) {
			// NULLS_LAST, therefore null is encoded as 1 - we can't break null ties
			return;
		}
		tie_col_offset++;
	}
	// if the tied strings are smaller than the prefix size, or are NULL, we don't need to break the ties
	char *prefix_chars = (char *)dataptr + start * sorting_state.ENTRY_SIZE + tie_col_offset;
	const char null_char = sorting_state.ORDER_TYPES[tie_col] == OrderType::ASCENDING ? 0 : -1;
	for (idx_t i = 0; i < StringStatistics::MAX_STRING_MINMAX_SIZE; i++) {
		if (prefix_chars[i] == null_char) {
			return;
		}
	}

	// fill pointer array for sorting
	auto ptr_block =
	    buffer_manager.Allocate(MaxValue((end - start) * sizeof(data_ptr_t), (idx_t)Storage::BLOCK_ALLOC_SIZE));
	auto entry_ptrs = (data_ptr_t *)ptr_block->node->buffer;
	for (idx_t i = start; i < end; i++) {
		entry_ptrs[i - start] = dataptr + i * sorting_state.ENTRY_SIZE;
	}

	// slow pointer-based sorting
	const int order = sorting_state.ORDER_TYPES[tie_col] == OrderType::DESCENDING ? -1 : 1;
	const idx_t sorting_size = sorting_state.ENTRY_SIZE - sizeof(idx_t);
	const idx_t *offsets = (idx_t *)offsets_ptr;
	std::sort(entry_ptrs, entry_ptrs + end - start,
	          [&var_dataptr, &offsets, &order, &sorting_size](const data_ptr_t l, const data_ptr_t r) {
		          return CompareStrings(l, r, var_dataptr, offsets, order, sorting_size);
	          });

	// re-order
	auto temp_block =
	    buffer_manager.Allocate(MaxValue((end - start) * sorting_state.ENTRY_SIZE, (idx_t)Storage::BLOCK_ALLOC_SIZE));
	data_ptr_t temp_ptr = temp_block->node->buffer;
	for (idx_t i = 0; i < end - start; i++) {
		memcpy(temp_ptr, entry_ptrs[i], sorting_state.ENTRY_SIZE);
		temp_ptr += sorting_state.ENTRY_SIZE;
	}
	memcpy(dataptr + start * sorting_state.ENTRY_SIZE, temp_block->node->buffer,
	       (end - start) * sorting_state.ENTRY_SIZE);

	// determine if there are still ties (if this is not the last column)
	if (tie_col < sorting_state.ORDER_TYPES.size() - 1) {
		data_ptr_t idx_ptr = dataptr + start * sorting_state.ENTRY_SIZE + sorting_size;

		idx_t current_idx = Load<idx_t>(idx_ptr);
		data_ptr_t current_ptr = var_dataptr + offsets[current_idx];
		uint32_t current_size = Load<uint32_t>(current_ptr);
		current_ptr += string_t::PREFIX_LENGTH;
		string_t current_val((const char *)current_ptr, current_size);
		for (idx_t i = 0; i < end - start - 1; i++) {
			idx_ptr += sorting_state.ENTRY_SIZE;

			// load next entry
			idx_t next_idx = Load<idx_t>(idx_ptr);
			data_ptr_t next_ptr = var_dataptr + offsets[next_idx];
			uint32_t next_size = Load<uint32_t>(next_ptr);
			next_ptr += string_t::PREFIX_LENGTH;
			string_t next_val((const char *)next_ptr, next_size);

			if (current_size != next_size) {
				// quick comparison: different length
				ties[start + i] = false;
			} else {
				// equal length: full comparison
				ties[start + i] = Equals::Operation<string_t>(current_val, next_val);
			}

			current_size = next_size;
			current_val = next_val;
		}
	}
}

static void BreakTies(BufferManager &buffer_manager, ContinuousBlock &cb, bool ties[], data_ptr_t dataptr,
                      const idx_t &count, const idx_t &tie_col, const SortingState &sorting_state) {
	D_ASSERT(!ties[count - 1]);
	auto &var_data_block = cb.var_sorting_chunks[tie_col]->data_blocks.back();
	auto &var_offsets_block = cb.var_sorting_chunks[tie_col]->offset_blocks.back();
	auto var_block_handle = buffer_manager.Pin(var_data_block.block);
	auto var_sizes_handle = buffer_manager.Pin(var_offsets_block.block);
	const data_ptr_t var_dataptr = var_block_handle->node->buffer;
	const data_ptr_t offsets_ptr = var_sizes_handle->node->buffer;

	for (idx_t i = 0; i < count; i++) {
		if (!ties[i]) {
			continue;
		}
		idx_t j;
		for (j = i; j < count; j++) {
			if (!ties[j]) {
				break;
			}
		}
		switch (sorting_state.TYPES[tie_col].InternalType()) {
		case PhysicalType::VARCHAR:
			BreakStringTies(buffer_manager, dataptr, i, j + 1, tie_col, ties, var_dataptr, offsets_ptr, sorting_state);
			break;
		default:
			throw NotImplementedException("Cannot sort variable size column with type %s",
			                              sorting_state.TYPES[tie_col].ToString());
		}
		i = j;
	}
}

static bool AnyTies(bool ties[], const idx_t &count) {
	D_ASSERT(!ties[count - 1]);
	bool any_ties = false;
	for (idx_t i = 0; i < count - 1; i++) {
		any_ties = any_ties || ties[i];
	}
	return any_ties;
}

static void ComputeTies(data_ptr_t dataptr, const idx_t &count, const idx_t &col_offset, const idx_t &tie_size,
                        bool ties[], const SortingState &sorting_state) {
	D_ASSERT(!ties[count - 1]);
	D_ASSERT(col_offset + tie_size <= sorting_state.ENTRY_SIZE - sizeof(idx_t));
	// align dataptr
	dataptr += col_offset;
	idx_t i = 0;
	for (; i + 7 < count - 1; i += 8) {
		// fixed size inner loop to allow unrolling
		for (idx_t j = 0; j < 8; j++) {
			ties[i + j] = ties[i + j] && memcmp(dataptr, dataptr + sorting_state.ENTRY_SIZE, tie_size) == 0;
			dataptr += sorting_state.ENTRY_SIZE;
		}
	}
	for (; i < count - 1; i++) {
		ties[i] = ties[i] && memcmp(dataptr, dataptr + sorting_state.ENTRY_SIZE, tie_size) == 0;
		dataptr += sorting_state.ENTRY_SIZE;
	}
	ties[count - 1] = false;
}

static void RadixSort(BufferManager &buffer_manager, data_ptr_t dataptr, const idx_t &count, const idx_t &col_offset,
                      const idx_t &sorting_size, const SortingState &sorting_state) {
	auto temp_block =
	    buffer_manager.Allocate(MaxValue(count * sorting_state.ENTRY_SIZE, (idx_t)Storage::BLOCK_ALLOC_SIZE));
	data_ptr_t temp = temp_block->node->buffer;
	bool swap = false;

	idx_t counts[256];
	uint8_t byte;
	for (idx_t offset = col_offset + sorting_size - 1; offset + 1 > col_offset; offset--) {
		// init to 0
		memset(counts, 0, sizeof(counts));
		// collect counts
		for (idx_t i = 0; i < count; i++) {
			byte = *(dataptr + i * sorting_state.ENTRY_SIZE + offset);
			counts[byte]++;
		}
		// compute offsets from counts
		for (idx_t val = 1; val < 256; val++) {
			counts[val] = counts[val] + counts[val - 1];
		}
		// re-order the data in temporary array
		for (idx_t i = count; i > 0; i--) {
			byte = *(dataptr + (i - 1) * sorting_state.ENTRY_SIZE + offset);
			memcpy(temp + (counts[byte] - 1) * sorting_state.ENTRY_SIZE, dataptr + (i - 1) * sorting_state.ENTRY_SIZE,
			       sorting_state.ENTRY_SIZE);
			counts[byte]--;
		}
		std::swap(dataptr, temp);
		swap = !swap;
	}
	// move data back to original buffer (if it was swapped)
	if (swap) {
		memcpy(temp, dataptr, count * sorting_state.ENTRY_SIZE);
	}
}

static void SubSortTiedTuples(BufferManager &buffer_manager, const data_ptr_t dataptr, const idx_t &count,
                              const idx_t &col_offset, const idx_t &sorting_size, bool ties[],
                              const SortingState &sorting_state) {
	D_ASSERT(!ties[count - 1]);
	for (idx_t i = 0; i < count; i++) {
		if (!ties[i]) {
			continue;
		}
		idx_t j;
		for (j = i + 1; j < count; j++) {
			if (!ties[j]) {
				break;
			}
		}
		RadixSort(buffer_manager, dataptr + i * sorting_state.ENTRY_SIZE, j - i + 1, col_offset, sorting_size,
		          sorting_state);
		i = j;
	}
}

static void SortInMemory(BufferManager &buffer_manager, ContinuousBlock &cb, const SortingState &sorting_state) {
	auto &block = cb.sorting_blocks.back();
	const auto &count = block.count;
	auto handle = buffer_manager.Pin(block.block);
	const auto dataptr = handle->node->buffer;

	// assign an index to each row
	idx_t sorting_size = sorting_state.ENTRY_SIZE - sizeof(idx_t);
	data_ptr_t idx_dataptr = dataptr + sorting_size;
	for (idx_t i = 0; i < count; i++) {
		Store<idx_t>(i, idx_dataptr);
		idx_dataptr += sorting_state.ENTRY_SIZE;
	}

	bool all_constant = true;
	for (idx_t i = 0; i < sorting_state.CONSTANT_SIZE.size(); i++) {
		all_constant = all_constant && sorting_state.CONSTANT_SIZE[i];
	}

	if (all_constant) {
		RadixSort(buffer_manager, dataptr, count, 0, sorting_size, sorting_state);
		return;
	}

	sorting_size = 0;
	idx_t col_offset = 0;
	unique_ptr<BufferHandle> ties_handle = nullptr;
	bool *ties = nullptr;
	const idx_t num_cols = sorting_state.CONSTANT_SIZE.size();
	for (idx_t i = 0; i < num_cols; i++) {
		sorting_size += sorting_state.COL_SIZE[i];
		if (sorting_state.CONSTANT_SIZE[i] && i < num_cols - 1) {
			// add columns to the sort until we reach a variable size column, or the last column
			continue;
		}

		if (!ties) {
			// this is the first sort
			RadixSort(buffer_manager, dataptr, count, col_offset, sorting_size, sorting_state);
			ties_handle = buffer_manager.Allocate(MaxValue(count, (idx_t)Storage::BLOCK_ALLOC_SIZE));
			ties = (bool *)ties_handle->node->buffer;
			std::fill_n(ties, count - 1, true);
			ties[count - 1] = false;
		} else {
			// for subsequent sorts, we subsort the tied tuples
			SubSortTiedTuples(buffer_manager, dataptr, count, col_offset, sorting_size, ties, sorting_state);
		}

		if (sorting_state.CONSTANT_SIZE[i] && i == num_cols - 1) {
			// all columns are sorted, no ties to break because last column is constant size
			break;
		}

		ComputeTies(dataptr, count, col_offset, sorting_size, ties, sorting_state);
		if (!AnyTies(ties, count)) {
			// no ties, so we stop sorting
			break;
		}

		BreakTies(buffer_manager, cb, ties, dataptr, count, i, sorting_state);
		if (!AnyTies(ties, count)) {
			// no more ties after tie-breaking
			break;
		}

		col_offset += sorting_size;
		sorting_size = 0;
	}
}

static void ReOrder(BufferManager &buffer_manager, ContinuousChunk &cc, data_ptr_t sorting_ptr,
                    const SortingState &sorting_state) {
	const idx_t &count = cc.data_blocks.back().count;

	auto &unordered_data_block = cc.data_blocks.back();
	auto unordered_data_handle = buffer_manager.Pin(unordered_data_block.block);
	const data_ptr_t unordered_data_ptr = unordered_data_handle->node->buffer;

	RowDataBlock reordered_data_block(buffer_manager, unordered_data_block.CAPACITY, unordered_data_block.ENTRY_SIZE);
	reordered_data_block.count = count;
	auto ordered_data_handle = buffer_manager.Pin(reordered_data_block.block);
	data_ptr_t ordered_data_ptr = ordered_data_handle->node->buffer;

	if (cc.CONSTANT_SIZE) {
		for (idx_t i = 0; i < count; i++) {
			memcpy(ordered_data_ptr, unordered_data_ptr + Load<idx_t>(sorting_ptr) * sorting_state.ENTRY_SIZE,
			       cc.ENTRY_SIZE);
			ordered_data_ptr += cc.ENTRY_SIZE;
			sorting_ptr += sorting_state.ENTRY_SIZE;
		}
	} else {
		// variable size data: we need offsets too
		reordered_data_block.byte_offset = unordered_data_block.byte_offset;
		auto &unordered_offset_block = cc.offset_blocks.back();
		auto unordered_offset_handle = buffer_manager.Pin(unordered_offset_block.block);
		idx_t *unordered_offsets = (idx_t *)unordered_offset_handle->node->buffer;

		RowDataBlock reordered_offset_block(buffer_manager, unordered_offset_block.CAPACITY,
		                                    unordered_offset_block.ENTRY_SIZE);
		reordered_offset_block.count = count;
		auto reordered_offset_handle = buffer_manager.Pin(reordered_offset_block.block);
		idx_t *reordered_offsets = (idx_t *)reordered_offset_handle->node->buffer;
		reordered_offsets[0] = 0;

		for (idx_t i = 0; i < count; i++) {
			idx_t index = Load<idx_t>(sorting_ptr);
			idx_t size = unordered_offsets[index + 1] - unordered_offsets[index];
			memcpy(ordered_data_ptr, unordered_data_ptr + unordered_offsets[index], size);
			ordered_data_ptr += size;
			reordered_offsets[i + 1] = reordered_offsets[i] + size;
			sorting_ptr += sorting_state.ENTRY_SIZE;
		}
		// replace offset block
		buffer_manager.UnregisterBlock(unordered_offset_block.block->BlockId(), true);
		cc.offset_blocks.clear();
		cc.offset_blocks.push_back(move(reordered_offset_block));
	}
	// replace data block
	buffer_manager.UnregisterBlock(unordered_data_block.block->BlockId(), true);
	cc.data_blocks.clear();
	cc.data_blocks.push_back(move(reordered_data_block));
}

//! Use the ordered sorting data to re-order the rest of the data
static void ReOrder(ClientContext &context, ContinuousBlock &cb, const SortingState &sorting_state,
                    const PayloadState &payload_state) {
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	auto sorting_handle = buffer_manager.Pin(cb.sorting_blocks.back().block);
	const data_ptr_t sorting_ptr = sorting_handle->node->buffer + sorting_state.ENTRY_SIZE - sizeof(idx_t);

	// re-order variable size sorting columns
	for (idx_t col_idx = 0; col_idx < sorting_state.CONSTANT_SIZE.size(); col_idx++) {
		if (!sorting_state.CONSTANT_SIZE[col_idx]) {
			ReOrder(buffer_manager, *cb.var_sorting_chunks[col_idx], sorting_ptr, sorting_state);
		}
	}
	// and the payload
	ReOrder(buffer_manager, *cb.payload_chunk, sorting_ptr, sorting_state);
}

void PhysicalOrder::SortLocalState(ClientContext &context, OrderLocalState &lstate, const SortingState &sorting_state,
                                   const PayloadState &payload_state) {
	const idx_t &count = lstate.sorting_block->count;
	D_ASSERT(count == lstate.payload_block->count);
	if (lstate.sorting_block->count == 0) {
		return;
	}

	// copy all data to ContinuousBlocks
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	auto cb = make_unique<ContinuousBlock>(buffer_manager, sorting_state);
	// fixed-size sorting data
	auto sorting_block = ConcatenateBlocks(buffer_manager, *lstate.sorting_block, false);
	cb->sorting_blocks.push_back(move(sorting_block));
	// variable size sorting columns
	for (idx_t i = 0; i < lstate.var_sorting_blocks.size(); i++) {
		unique_ptr<ContinuousChunk> cc = nullptr;
		if (!sorting_state.CONSTANT_SIZE[i]) {
			cc = make_unique<ContinuousChunk>(buffer_manager, sorting_state.CONSTANT_SIZE[i]);
			auto &row_chunk = *lstate.var_sorting_blocks[i];
			auto new_block = ConcatenateBlocks(buffer_manager, row_chunk, true);
			auto &sizes_chunk = *lstate.var_sorting_sizes[i];
			auto offsets_block = SizesToOffsets(buffer_manager, sizes_chunk);
			cc->data_blocks.push_back(move(new_block));
			cc->offset_blocks.push_back(move(offsets_block));
		}
		cb->var_sorting_chunks.push_back(move(cc));
	}
	// payload data
	auto payload_cc =
	    make_unique<ContinuousChunk>(buffer_manager, !payload_state.HAS_VARIABLE_SIZE, payload_state.ENTRY_SIZE);
	auto payload_block = ConcatenateBlocks(buffer_manager, *lstate.payload_block, payload_state.HAS_VARIABLE_SIZE);
	payload_cc->data_blocks.push_back(move(payload_block));
	if (payload_state.HAS_VARIABLE_SIZE) {
		auto offsets_block = SizesToOffsets(buffer_manager, *lstate.sizes_block);
		payload_cc->offset_blocks.push_back(move(offsets_block));
	}
	cb->payload_chunk = move(payload_cc);

	// now perform the actual sort
	SortInMemory(buffer_manager, *cb, sorting_state);

	// re-order before the merge sort
	ReOrder(context, *cb, sorting_state, payload_state);

	// add the sorted block to the global state
	lstate.sorted_blocks.push_back(move(cb));
}


void PhysicalOrder::Finalize(Pipeline &pipeline, ClientContext &context, unique_ptr<GlobalOperatorState> state_p) {
	this->sink_state = move(state_p);
	auto &state = (OrderGlobalState &)*this->sink_state;

	if (state.sorted_blocks.empty()) {
		return;
	}

	auto &cb = *state.sorted_blocks.back();
	const idx_t &count = cb.sorting_blocks.back().count;
	D_ASSERT(count == cb.payload_chunk->data_blocks.back().count);
	state.total_count = count;

	// TODO: now we have a bunch of sorted data in the global state: ContinuousBlocks
	// TODO: all we need to do now is have a merge procedure
}

//===--------------------------------------------------------------------===//
// GetChunkInternal
//===--------------------------------------------------------------------===//
idx_t PhysicalOrder::MaxThreads(ClientContext &context) {
	if (this->sink_state) {
		auto &state = (OrderGlobalState &)*this->sink_state;
		return state.sorted_blocks.back()->payload_chunk->data_blocks.back().count / STANDARD_VECTOR_SIZE + 1;
	} else {
		return estimated_cardinality / STANDARD_VECTOR_SIZE + 1;
	}
}

class OrderParallelState : public ParallelState {
public:
	OrderParallelState() : entry_idx(0) {
	}
	idx_t entry_idx;
	std::mutex lock;
};

unique_ptr<ParallelState> PhysicalOrder::GetParallelState() {
	auto result = make_unique<OrderParallelState>();
	return move(result);
}

class PhysicalOrderOperatorState : public PhysicalOperatorState {
public:
	PhysicalOrderOperatorState(PhysicalOperator &op, PhysicalOperator *child)
	    : PhysicalOperatorState(op, child), initialized(false), entry_idx(0), count(-1) {
	}
	ParallelState *parallel_state;
	bool initialized;

	unique_ptr<BufferHandle> sorting_handle = nullptr;
	unique_ptr<BufferHandle> payload_handle;
	unique_ptr<BufferHandle> offsets_handle;

	data_ptr_t key_locations[STANDARD_VECTOR_SIZE];
	data_ptr_t validitymask_locations[STANDARD_VECTOR_SIZE];

	idx_t entry_idx;
	idx_t count;
};

unique_ptr<PhysicalOperatorState> PhysicalOrder::GetOperatorState() {
	return make_unique<PhysicalOrderOperatorState>(*this, children[0].get());
}

static void Scan(ClientContext &context, DataChunk &chunk, PhysicalOrderOperatorState &state,
                 const SortingState &sorting_state, const PayloadState &payload_state, const idx_t offset,
                 const idx_t next) {
	if (offset >= state.count) {
		return;
	}

	const data_ptr_t payl_dataptr = state.payload_handle->node->buffer;
	if (payload_state.HAS_VARIABLE_SIZE) {
		const idx_t *offsets = (idx_t *)state.offsets_handle->node->buffer;
		for (idx_t i = 0; i < next; i++) {
			state.validitymask_locations[i] = payl_dataptr + offsets[i];
			state.key_locations[i] = state.validitymask_locations[i] + payload_state.VALIDITYMASK_SIZE;
		}
	} else {
		for (idx_t i = 0; i < next; i++) {
			state.validitymask_locations[i] = payl_dataptr;
			state.key_locations[i] = state.validitymask_locations[i] + payload_state.VALIDITYMASK_SIZE;
		}
	}

	// deserialize the payload data
	for (idx_t payl_col = 0; payl_col < chunk.ColumnCount(); payl_col++) {
		RowChunk::DeserializeIntoVector(chunk.data[payl_col], next, payl_col, state.key_locations,
		                                state.validitymask_locations);
	}
	chunk.SetCardinality(next);
	chunk.Verify();
}

void PhysicalOrder::GetChunkInternal(ExecutionContext &context, DataChunk &chunk, PhysicalOperatorState *state_p) {
	auto &state = *reinterpret_cast<PhysicalOrderOperatorState *>(state_p);
	auto &gstate = (OrderGlobalState &)*this->sink_state;
	const auto &sorting_state = *gstate.sorting_state;
	const auto &payload_state = *gstate.payload_state;

	if (gstate.sorted_blocks.empty()) {
		return;
	}

	if (!state.initialized) {
		// initialize operator state
		auto &cb = *gstate.sorted_blocks.back();
		state.count = cb.sorting_blocks.back().count;

		auto &buffer_manager = BufferManager::GetBufferManager(context.client);
		if (state.count > 0) {
			state.payload_handle = buffer_manager.Pin(cb.payload_chunk->data_blocks.back().block);
			if (payload_state.HAS_VARIABLE_SIZE) {
				state.offsets_handle = buffer_manager.Pin(cb.payload_chunk->offset_blocks.back().block);
			}
		}
		// initialize parallel state (if any)
		state.parallel_state = nullptr;
		auto &task = context.task;
		// check if there is any parallel state to fetch
		state.parallel_state = nullptr;
		auto task_info = task.task_info.find(this);
		if (task_info != task.task_info.end()) {
			// parallel scan init
			state.parallel_state = task_info->second;
		}
		state.initialized = true;
	}

	if (!state.parallel_state) {
		// sequential scan
		const idx_t next = MinValue((idx_t)STANDARD_VECTOR_SIZE, state.count - state.entry_idx);
		Scan(context.client, chunk, state, sorting_state, payload_state, state.entry_idx, next);
		state.entry_idx += STANDARD_VECTOR_SIZE;
		if (chunk.size() != 0) {
			return;
		}
	} else {
		// parallel scan
		auto &parallel_state = *reinterpret_cast<OrderParallelState *>(state.parallel_state);
		do {
			idx_t offset;
			idx_t next;
			{
				lock_guard<mutex> parallel_lock(parallel_state.lock);
				offset = parallel_state.entry_idx;
				next = MinValue((idx_t)STANDARD_VECTOR_SIZE, state.count - offset);
				parallel_state.entry_idx += next;
			}
			Scan(context.client, chunk, state, sorting_state, payload_state, offset, next);
			if (chunk.size() == 0) {
				break;
			} else {
				return;
			}
		} while (true);
	}
	D_ASSERT(chunk.size() == 0);
}

string PhysicalOrder::ParamsToString() const {
	string result;
	for (idx_t i = 0; i < orders.size(); i++) {
		if (i > 0) {
			result += "\n";
		}
		result += orders[i].expression->ToString() + " ";
		result += orders[i].type == OrderType::DESCENDING ? "DESC" : "ASC";
	}
	return result;
}

} // namespace duckdb
