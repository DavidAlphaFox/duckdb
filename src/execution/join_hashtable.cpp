#include "execution/join_hashtable.hpp"

#include "common/exception.hpp"
#include "common/types/static_vector.hpp"
#include "common/vector_operations/vector_operations.hpp"
#include "common/types/null_value.hpp"

using namespace duckdb;
using namespace std;

using ScanStructure = JoinHashTable::ScanStructure;

JoinHashTable::JoinHashTable(vector<JoinCondition> &conditions, vector<TypeId> build_types, JoinType type,
                             bool null_values_are_equal, size_t initial_capacity, bool parallel)
    : build_serializer(build_types), build_types(build_types), equality_size(0), condition_size(0), build_size(0),
      entry_size(0), tuple_size(0), join_type(type), has_null(false), capacity(0), count(0), parallel(parallel), null_values_are_equal(null_values_are_equal) {
	for (auto &condition : conditions) {
		assert(condition.left->return_type == condition.right->return_type);
		auto type = condition.left->return_type;
		auto type_size = GetTypeIdSize(type);
		if (condition.comparison == ExpressionType::COMPARE_EQUAL) {
			// all equality conditions should be at the front
			// all other conditions at the back
			// this assert checks that
			assert(equality_types.size() == condition_types.size());
			equality_types.push_back(type);
			equality_size += type_size;
		}
		predicates.push_back(condition.comparison);

		condition_types.push_back(type);
		condition_size += type_size;
	}
	// at least one equality is necessary
	assert(equality_types.size() > 0);
	equality_serializer.Initialize(equality_types);
	condition_serializer.Initialize(condition_types);

	if (type == JoinType::ANTI || type == JoinType::SEMI || type == JoinType::MARK) {
		// for ANTI, SEMI and MARK join, we only need to store the keys
		build_size = 0;
	} else {
		// otherwise we need to store the entire build side for reconstruction
		// purposes
		for (size_t i = 0; i < build_types.size(); i++) {
			build_size += GetTypeIdSize(build_types[i]);
		}
	}
	tuple_size = condition_size + build_size;
	entry_size = tuple_size + sizeof(void *);
	Resize(initial_capacity);
}

void JoinHashTable::InsertHashes(Vector &hashes, uint8_t *key_locations[]) {
	assert(hashes.type == TypeId::POINTER);
	hashes.Flatten();

	// use modulo to get position in array (FIXME: can be done more efficiently)
	VectorOperations::ModuloInPlace(hashes, capacity);

	auto pointers = hashed_pointers.get();
	auto indices = (uint64_t *)hashes.data;
	// now fill in the entries
	for (size_t i = 0; i < hashes.count; i++) {
		auto index = indices[i];
		// set prev in current key to the value (NOTE: this will be nullptr if
		// there is none)
		auto prev_pointer = (uint8_t **)(key_locations[i] + tuple_size);
		*prev_pointer = pointers[index];

		// set pointer to current tuple
		pointers[index] = key_locations[i];
	}
}

void JoinHashTable::Resize(size_t size) {
	if (size <= capacity) {
		throw Exception("Cannot downsize a hash table!");
	}
	capacity = size;

	hashed_pointers = unique_ptr<uint8_t *[]>(new uint8_t *[capacity]);
	memset(hashed_pointers.get(), 0, capacity * sizeof(uint8_t *));

	if (count > 0) {
		// we have entries, need to rehash the pointers
		// first reset all chain pointers to the nullptr
		// we could do this by actually following the chains in the
		// hashed_pointers as well might be more or less efficient depending on
		// length of chains?
		auto node = head.get();
		while (node) {
			// scan all the entries in this node
			auto entry_pointer = (uint8_t *)node->data.get() + tuple_size;
			for (size_t i = 0; i < node->count; i++) {
				// reset chain pointer
				auto prev_pointer = (uint8_t **)entry_pointer;
				*prev_pointer = nullptr;
				// move to next entry
				entry_pointer += entry_size;
			}
			node = node->prev.get();
		}

		// now rehash the entries
		DataChunk keys;
		keys.Initialize(equality_types);

		Vector entry_pointers(TypeId::POINTER, true, true);
		auto deserialize_locations = (uint8_t **)entry_pointers.data;
		uint8_t *key_locations[STANDARD_VECTOR_SIZE];

		node = head.get();
		while (node) {
			// scan all the entries in this node
			auto dataptr = node->data.get();
			for (size_t i = 0; i < node->count; i++) {
				// key is stored at the start
				key_locations[i] = deserialize_locations[i] = dataptr;
				// move to next entry
				dataptr += entry_size;
			}
			entry_pointers.count = node->count;

			// reconstruct the keys chunk from the stored entries
			// we only reconstruct the keys that are part of the equality
			// comparison as these are the ones that are used to compute the
			// hash
			equality_serializer.Deserialize(entry_pointers, keys);

			// create the hash
			StaticVector<uint64_t> hashes;
			keys.Hash(hashes);

			// re-insert the entries
			InsertHashes(hashes, key_locations);

			// move to the next node
			node = node->prev.get();
		}
	}
}

void JoinHashTable::Hash(DataChunk &keys, Vector &hashes) {
	VectorOperations::Hash(keys.data[0], hashes);
	for (size_t i = 1; i < equality_types.size(); i++) {
		VectorOperations::CombineHash(hashes, keys.data[i]);
	}
}

static size_t CreateNotNullSelVector(DataChunk &keys, sel_t *not_null_sel_vector) {
	sel_t *sel_vector = keys.data[0].sel_vector;
	size_t result_count = keys.size();
	// first we loop over all the columns and figure out where the 
	for(size_t i = 0; i < keys.column_count; i++) {
		keys.data[i].sel_vector = sel_vector;
		keys.data[i].count = result_count;
		result_count = Vector::NotNullSelVector(keys.data[i], not_null_sel_vector, sel_vector, nullptr);
	}
	// now assign the final count and selection vectors
	for(size_t i = 0; i < keys.column_count; i++) {
		keys.data[i].sel_vector = sel_vector;
		keys.data[i].count = result_count;
	}
	keys.sel_vector = sel_vector;
	return result_count;
}

template<class T>
void FillNullMask(Vector &v) {
	auto data = (T*) v.data;
	VectorOperations::Exec(v, [&](size_t i, size_t k) {
		if (v.nullmask[i]) {
			data[i] = NullValue<T>();
		}
	});
	v.nullmask.reset();
}

static void FillNullMask(Vector& v) {
	if (!v.nullmask.any()) {
		// no NULL values, skip
		return;
	}
	switch(v.type) {
	case TypeId::BOOLEAN:
	case TypeId::TINYINT:
		FillNullMask<int8_t>(v);
		break;
	case TypeId::SMALLINT:
		FillNullMask<int16_t>(v);
		break;
	case TypeId::DATE:
	case TypeId::INTEGER:
		FillNullMask<int32_t>(v);
		break;
	case TypeId::TIMESTAMP:
	case TypeId::BIGINT:
		FillNullMask<int64_t>(v);
		break;
	case TypeId::DECIMAL:
		FillNullMask<double>(v);
		break;
	case TypeId::VARCHAR:
		FillNullMask<const char*>(v);
		break;
	default:
		throw NotImplementedException("Type not implemented for HT null mask");
	}
}

void JoinHashTable::Build(DataChunk &keys, DataChunk &payload) {
	if (keys.size() == 0) {
		return;
	}
	assert(keys.size() == payload.size());
	sel_t not_null_sel_vector[STANDARD_VECTOR_SIZE];
	if (join_type == JoinType::MARK && correlated_mark_join_info.correlated_types.size() > 0) {
		auto &info = correlated_mark_join_info;
		// Correlated MARK join
		// first push the values into the correlated_counts
		assert(info.correlated_counts);
		for(size_t i = 0; i < info.correlated_types.size(); i++) {
			info.group_chunk.data[i].Reference(keys.data[i]);
		}
		info.payload_chunk.data[0].Reference(keys.data[info.correlated_types.size()]);
		info.payload_chunk.data[1].Reference(keys.data[info.correlated_types.size()]);
		info.payload_chunk.data[0].type = info.payload_chunk.data[1].type = TypeId::BIGINT;
		info.payload_chunk.sel_vector = info.group_chunk.sel_vector = info.group_chunk.data[0].sel_vector;
		info.correlated_counts->AddChunk(info.group_chunk, info.payload_chunk);
		// the correlated columns have equivalent NULL values
		// then fill in the NULL mask of the duplicate eliminated columns
		for(size_t i = 0; i < info.correlated_types.size(); i++) {
			FillNullMask(keys.data[i]);
		}
	}
	if (!null_values_are_equal) {
		// first create a selection vector of the non-null values in the keys
		// because in a join, any NULL value can never find a matching tuple
		size_t initial_keys_size = keys.size();
		size_t not_null_count;
		not_null_count = CreateNotNullSelVector(keys, not_null_sel_vector);
		if (not_null_count != initial_keys_size) {
			// the hashtable contains null values in the keys!
			// set the property in the HT to true
			// this is required for the mark join
			has_null = true;
			// now assign the new count and sel_vector to the payload as well
			for(size_t i = 0; i < payload.column_count; i++) {
				payload.data[i].count = not_null_count;
				payload.data[i].sel_vector = keys.data[0].sel_vector;
			}
			payload.sel_vector = keys.data[0].sel_vector;
		}
		if (not_null_count == 0) {
			return;
		}
	} else if (join_type != JoinType::MARK) {
		// in this join, NULL values are equivalent!
		// we replace NULLs in the mask with NullValue<T> inside the vector
		for(size_t i = 0; i < keys.column_count; i++) {
			FillNullMask(keys.data[i]);
		}
	}
	// resize at 50% capacity, also need to fit the entire vector
	if (parallel) {
		parallel_lock.lock();
	}
	if (count + keys.size() > capacity / 2) {
		Resize(capacity * 2);
	}
	count += keys.size();
	// move strings to the string heap
	keys.MoveStringsToHeap(string_heap);
	payload.MoveStringsToHeap(string_heap);

	if (parallel) {
		parallel_lock.unlock();
	}
	
	// get the locations of where to serialize the keys and payload columns
	uint8_t *key_locations[STANDARD_VECTOR_SIZE];
	uint8_t *tuple_locations[STANDARD_VECTOR_SIZE];
	auto node = make_unique<Node>(entry_size, keys.size());
	auto dataptr = node->data.get();
	for (size_t i = 0; i < keys.size(); i++) {
		// key is stored at the start
		key_locations[i] = dataptr;
		// after that the build-side tuple is stored
		tuple_locations[i] = dataptr + condition_size;
		dataptr += entry_size;
	}
	node->count = keys.size();

	// serialize the values to these locations
	// we serialize all the condition variables here
	condition_serializer.Serialize(keys, key_locations);
	if (build_size > 0) {
		build_serializer.Serialize(payload, tuple_locations);
	}

	// hash the keys and obtain an entry in the list
	// note that we only hash the keys used in the equality comparison
	StaticVector<uint64_t> hashes;
	Hash(keys, hashes);

	if (parallel) {
		// obtain lock
		parallel_lock.lock();
	}
	InsertHashes(hashes, key_locations);
	// store the new node as the head
	node->prev = move(head);
	head = move(node);
	if (parallel) {
		parallel_lock.unlock();
	}
}

unique_ptr<ScanStructure> JoinHashTable::Probe(DataChunk &keys) {
	assert(!keys.sel_vector); // should be flattened before

	if (join_type == JoinType::MARK && correlated_mark_join_info.correlated_types.size() > 0) {
		auto &info = correlated_mark_join_info;
		// MARK join: the correlated columns have equivalent NULL values
		assert(info.correlated_types.size() + 1 == keys.column_count);
		for(size_t i = 0; i < info.correlated_types.size(); i++) {
			FillNullMask(keys.data[i]);
		}
	} else if (null_values_are_equal) {
		// NULL values are equal, fill NULL values with NullValue<T> in the keys
		for(size_t i = 0; i < keys.column_count; i++) {
			FillNullMask(keys.data[i]);
		}
	}

	// scan structure
	auto ss = make_unique<ScanStructure>(*this);
	// first hash all the keys to do the lookup
	StaticVector<uint64_t> hashes;
	Hash(keys, hashes);

	// use modulo to get index in array
	VectorOperations::ModuloInPlace(hashes, capacity);

	// now create the initial pointers from the hashes
	auto ptrs = (uint8_t **)ss->pointers.data;
	auto indices = (uint64_t *)hashes.data;
	for (size_t i = 0; i < hashes.count; i++) {
		auto index = indices[i];
		ptrs[i] = hashed_pointers[index];
	}
	ss->pointers.count = hashes.count;

	switch (join_type) {
	case JoinType::SEMI:
	case JoinType::ANTI:
	case JoinType::LEFT:
	case JoinType::SINGLE:
	case JoinType::MARK:
		// initialize all tuples with found_match to false
		memset(ss->found_match, 0, sizeof(ss->found_match));
	case JoinType::INNER: {
		// create the selection vector linking to only non-empty entries
		size_t count = 0;
		for (size_t i = 0; i < ss->pointers.count; i++) {
			if (ptrs[i]) {
				ss->sel_vector[count++] = i;
			}
		}
		ss->pointers.sel_vector = ss->sel_vector;
		ss->pointers.count = count;
		break;
	}
	default:
		throw NotImplementedException("Unimplemented join type for hash join");
	}

	return ss;
}

ScanStructure::ScanStructure(JoinHashTable &ht) : ht(ht), finished(false) {
	pointers.Initialize(TypeId::POINTER, false);
	build_pointer_vector.Initialize(TypeId::POINTER, false);
	pointers.sel_vector = this->sel_vector;
}

void ScanStructure::Next(DataChunk &keys, DataChunk &left, DataChunk &result) {
	assert(!left.sel_vector && !keys.sel_vector); // should be flattened before
	if (finished) {
		return;
	}

	switch (ht.join_type) {
	case JoinType::INNER:
		NextInnerJoin(keys, left, result);
		break;
	case JoinType::SEMI:
		NextSemiJoin(keys, left, result);
		break;
	case JoinType::MARK:
		NextMarkJoin(keys, left, result);
		break;
	case JoinType::ANTI:
		NextAntiJoin(keys, left, result);
		break;
	case JoinType::LEFT:
		NextLeftJoin(keys, left, result);
		break;
	case JoinType::SINGLE:
		NextSingleJoin(keys, left, result);
		break;
	default:
		throw Exception("Unhandled join type in JoinHashTable");
	}
}

void ScanStructure::ResolvePredicates(DataChunk &keys, Vector &final_result) {
	Vector current_pointers;
	current_pointers.Reference(pointers);

	sel_t temporary_selection_vector[STANDARD_VECTOR_SIZE];

	auto old_sel_vector = current_pointers.sel_vector;
	auto old_count = current_pointers.count;

	for (size_t i = 0; i < ht.predicates.size(); i++) {
		// gather the data from the pointers
		Vector ht_data(keys.data[i].type, true, false);
		ht_data.sel_vector = current_pointers.sel_vector;
		ht_data.count = current_pointers.count;
		// we don't check for NULL values in the keys because either
		// (1) NULL values will have been filtered out before (null_values_are_equal = false) or
		// (2) we want NULL=NULL to be true (null_values_are_equal = true)
		VectorOperations::Gather::Set(current_pointers, ht_data, false);

		// set the selection vector
		size_t old_count = keys.data[i].count;
		assert(!keys.data[i].sel_vector);
		keys.data[i].sel_vector = ht_data.sel_vector;
		keys.data[i].count = ht_data.count;

		// perform the comparison expression
		switch (ht.predicates[i]) {
		case ExpressionType::COMPARE_EQUAL:
			VectorOperations::Equals(keys.data[i], ht_data, final_result);
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			VectorOperations::GreaterThan(keys.data[i], ht_data, final_result);
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			VectorOperations::GreaterThanEquals(keys.data[i], ht_data, final_result);
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			VectorOperations::LessThan(keys.data[i], ht_data, final_result);
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			VectorOperations::LessThanEquals(keys.data[i], ht_data, final_result);
			break;
		case ExpressionType::COMPARE_NOTEQUAL:
			VectorOperations::NotEquals(keys.data[i], ht_data, final_result);
			break;
		default:
			throw NotImplementedException("Unimplemented comparison type for join");
		}
		// reset the selection vector
		keys.data[i].sel_vector = nullptr;
		keys.data[i].count = old_count;

		// now based on the result recreate the selection vector for the next
		// step so we can skip unnecessary comparisons in the next phase
		if (i != ht.predicates.size() - 1) {
			size_t new_count = 0;
			VectorOperations::ExecType<bool>(final_result, [&](bool match, size_t index, size_t k) {
				if (match) {
					temporary_selection_vector[new_count++] = index;
				}
			});
			current_pointers.sel_vector = temporary_selection_vector;
			current_pointers.count = new_count;
		}
		// move all the pointers to the next element
		VectorOperations::AddInPlace(pointers, GetTypeIdSize(keys.data[i].type));
	}
	final_result.sel_vector = old_sel_vector;
	final_result.count = old_count;
}

size_t ScanStructure::ScanInnerJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	StaticVector<bool> comparison_result;
	size_t result_count = 0;
	do {
		auto build_pointers = (uint8_t **)build_pointer_vector.data;

		// resolve the predicates for all the pointers
		ResolvePredicates(keys, comparison_result);

		auto ptrs = (uint8_t **)pointers.data;
		// after doing all the comparisons we loop to find all the actual matches
		result_count = 0;
		VectorOperations::ExecType<bool>(comparison_result, [&](bool match, size_t index, size_t k) {
			if (match) {
				found_match[index] = true;
				result.owned_sel_vector[result_count] = index;
				build_pointers[result_count] = ptrs[index];
				result_count++;
			}
		});

		// finally we chase the pointers for the next iteration
		size_t new_count = 0;
		VectorOperations::Exec(pointers, [&](size_t index, size_t k) {
			auto prev_pointer = (uint8_t **)(ptrs[index] + ht.build_size);
			ptrs[index] = *prev_pointer;
			if (ptrs[index]) {
				// if there is a next pointer, we keep this entry
				// otherwise the entry is removed from the sel_vector
				sel_vector[new_count++] = index;
			}
		});
		pointers.count = new_count;
	} while (pointers.count > 0 && result_count == 0);
	return result_count;
}

void ScanStructure::NextInnerJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	assert(result.column_count == left.column_count + ht.build_types.size());
	if (pointers.count == 0) {
		// no pointers left to chase
		return;
	}

	size_t result_count = ScanInnerJoin(keys, left, result);
	if (result_count > 0) {
		// matches were found
		// construct the result
		result.sel_vector = result.owned_sel_vector;
		build_pointer_vector.count = result_count;

		// reference the columns of the left side from the result
		for (size_t i = 0; i < left.column_count; i++) {
			result.data[i].Reference(left.data[i]);
			result.data[i].sel_vector = result.sel_vector;
			result.data[i].count = result_count;
		}
		// apply the selection vector
		// now fetch the right side data from the HT
		for (size_t i = 0; i < ht.build_types.size(); i++) {
			auto &vector = result.data[left.column_count + i];
			vector.sel_vector = result.sel_vector;
			vector.count = result_count;
			ht.build_serializer.DeserializeColumn(build_pointer_vector, i, result.data[left.column_count + i]);
		}
	}
}

void ScanStructure::ScanKeyMatches(DataChunk &keys) {
	// the semi-join, anti-join and mark-join we handle a differently from the inner join
	// since there can be at most STANDARD_VECTOR_SIZE results
	// we handle the entire chunk in one call to Next().
	// for every pointer, we keep chasing pointers and doing comparisons.
	// this results in a boolean array indicating whether or not the tuple has a match
	StaticVector<bool> comparison_result;
	while (pointers.count > 0) {
		// resolve the predicates for the current set of pointers
		ResolvePredicates(keys, comparison_result);

		// after doing all the comparisons we loop to find all the matches
		auto ptrs = (uint8_t **)pointers.data;
		size_t new_count = 0;
		VectorOperations::ExecType<bool>(comparison_result, [&](bool match, size_t index, size_t k) {
			if (match) {
				// found a match, set the entry to true
				// after this we no longer need to check this entry
				found_match[index] = true;
			} else {
				// did not find a match, keep on looking for this entry
				// first check if there is a next entry
				auto prev_pointer = (uint8_t **)(ptrs[index] + ht.build_size);
				ptrs[index] = *prev_pointer;
				if (ptrs[index]) {
					// if there is a next pointer, we keep this entry
					sel_vector[new_count++] = index;
				}
			}
		});
		pointers.count = new_count;
	}
}

template <bool MATCH>
void ScanStructure::NextSemiOrAntiJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	assert(left.column_count == result.column_count);
	assert(keys.size() == left.size());
	// create the selection vector from the matches that were found
	size_t result_count = 0;
	for (size_t i = 0; i < keys.size(); i++) {
		if (found_match[i] == MATCH) {
			// part of the result
			result.owned_sel_vector[result_count++] = i;
		}
	}
	// construct the final result
	if (result_count > 0) {
		// we only return the columns on the left side
		// project them using the result selection vector
		result.sel_vector = result.owned_sel_vector;
		// reference the columns of the left side from the result
		for (size_t i = 0; i < left.column_count; i++) {
			result.data[i].Reference(left.data[i]);
			result.data[i].sel_vector = result.sel_vector;
			result.data[i].count = result_count;
		}
	} else {
		assert(result.size() == 0);
	}
}

void ScanStructure::NextSemiJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	// first scan for key matches
	ScanKeyMatches(keys);
	// then construct the result from all tuples with a match
	NextSemiOrAntiJoin<true>(keys, left, result);

	finished = true;
}

void ScanStructure::NextAntiJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	// first scan for key matches
	ScanKeyMatches(keys);
	// then construct the result from all tuples that did not find a match
	NextSemiOrAntiJoin<false>(keys, left, result);

	finished = true;
}

namespace duckdb {
void ConstructMarkJoinResult(DataChunk &join_keys, DataChunk &child, DataChunk &result, bool found_match[], bool right_has_null) {
	// for the initial set of columns we just reference the left side
	for(size_t i = 0; i < child.column_count; i++) {
		result.data[i].Reference(child.data[i]);
	}
	// create the result matching vector
	auto &result_vector = result.data[child.column_count];
	result_vector.count = child.size();
	// first we set the NULL values from the join keys
	// if there is any NULL in the keys, the result is NULL
	if (join_keys.column_count > 0) {
		result_vector.nullmask = join_keys.data[0].nullmask;
		for(size_t i = 1; i < join_keys.column_count; i++) {
			result_vector.nullmask |= join_keys.data[i].nullmask;
		}
	}
	// now set the remaining entries to either true or false based on whether a match was found
	auto bool_result = (bool*) result_vector.data;
	for(size_t i = 0; i < result_vector.count; i++) {
		bool_result[i] = found_match[i];
	}
	// if the right side contains NULL values, the result of any FALSE becomes NULL
	if (right_has_null) {
		for(size_t i = 0; i < result_vector.count; i++) {
			if (!bool_result[i]) {
				result_vector.nullmask[i] = true;
			}
		}
	}
}
}

void ScanStructure::NextMarkJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	assert(result.column_count == left.column_count + 1);
	assert(result.data[left.column_count].type == TypeId::BOOLEAN);
	assert(!left.sel_vector);
	// this method should only be called for a non-empty HT
	assert(ht.count > 0);

	ScanKeyMatches(keys);
	if (ht.correlated_mark_join_info.correlated_types.size() == 0) {
		ConstructMarkJoinResult(keys, left, result, found_match, ht.has_null);
	} else {
		auto &info = ht.correlated_mark_join_info;
		// there are correlated columns
		// first we fetch the counts from the aggregate hashtable corresponding to these entries
		assert(keys.column_count == info.group_chunk.column_count + 1);
		for(size_t i = 0; i < info.group_chunk.column_count; i++) {
			info.group_chunk.data[i].Reference(keys.data[i]);
		}
		info.group_chunk.sel_vector = keys.sel_vector;
		info.correlated_counts->FetchAggregates(info.group_chunk, info.result_chunk);
		assert(!info.result_chunk.sel_vector);

		// for the initial set of columns we just reference the left side
		for(size_t i = 0; i < left.column_count; i++) {
			result.data[i].Reference(left.data[i]);
		}
		// create the result matching vector
		auto &result_vector = result.data[left.column_count];
		result_vector.count = result.data[0].count;
		// first set the nullmask based on whether or not there were NULL values in the join key
		result_vector.nullmask = keys.data[keys.column_count - 1].nullmask;
		
		auto bool_result = (bool*) result_vector.data;
		auto count_star = (int64_t*) info.result_chunk.data[0].data;
		auto count = (int64_t*) info.result_chunk.data[1].data;
		// set the entries to either true or false based on whether a match was found
		for(size_t i = 0; i < result_vector.count; i++) {
			assert(count_star[i] >= count[i]);
			bool_result[i] = found_match[i];
			if (!bool_result[i] && count_star[i] > count[i]) {
				// RHS has NULL value and result is false:, set to null
				result_vector.nullmask[i] = true;
			}
			if (count_star[i] == 0) {
				// count == 0, set nullmask to false (we know the result is false now)
				result_vector.nullmask[i] = false;
			}
		}
	}
	finished = true;
}

void ScanStructure::NextLeftJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	// a LEFT OUTER JOIN is identical to an INNER JOIN except all tuples that do
	// not have a match must return at least one tuple (with the right side set
	// to NULL in every column)
	NextInnerJoin(keys, left, result);
	if (result.size() == 0) {
		// no entries left from the normal join
		// fill in the result of the remaining left tuples
		// together with NULL values on the right-hand side
		size_t remaining_count = 0;
		for (size_t i = 0; i < left.size(); i++) {
			if (!found_match[i]) {
				result.owned_sel_vector[remaining_count++] = i;
			}
		}
		if (remaining_count > 0) {
			// have remaining tuples
			// first set the left side
			result.sel_vector = result.owned_sel_vector;
			size_t i = 0;
			for (; i < left.column_count; i++) {
				result.data[i].Reference(left.data[i]);
				result.data[i].sel_vector = result.sel_vector;
				result.data[i].count = remaining_count;
			}
			// now set the right side to NULL
			for (; i < result.column_count; i++) {
				result.data[i].nullmask.set();
				result.data[i].sel_vector = result.sel_vector;
				result.data[i].count = remaining_count;
			}
		}
		finished = true;
	}
}

void ScanStructure::NextSingleJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	// single join
	// this join is similar to the semi join except that
	// (1) we actually return data from the RHS and
	// (2) we return NULL for that data if there is no match
	StaticVector<bool> comparison_result;

	auto build_pointers = (uint8_t **)build_pointer_vector.data;
	size_t result_count = 0;
	sel_t result_sel_vector[STANDARD_VECTOR_SIZE];
	while(pointers.count > 0) {
		// resolve the predicates for all the pointers
		ResolvePredicates(keys, comparison_result);
		
		auto ptrs = (uint8_t **)pointers.data;
		// after doing all the comparisons we loop to find all the actual matches
		VectorOperations::ExecType<bool>(comparison_result, [&](bool match, size_t index, size_t k) {
			if (match) {
				// found a match for this index
				// set the build_pointers to this position
				found_match[index] = true;
				build_pointers[result_count] = ptrs[index];
				result_sel_vector[result_count] = index;
				result_count++;
			}
		});

		// finally we chase the pointers for the next iteration
		size_t new_count = 0;
		VectorOperations::Exec(pointers, [&](size_t index, size_t k) {
			auto prev_pointer = (uint8_t **)(ptrs[index] + ht.build_size);
			ptrs[index] = *prev_pointer;
			if (ptrs[index] && !found_match[index]) {
				// if there is a next pointer, and we have not found a match yet, we keep this entry
				pointers.sel_vector[new_count++] = index;
			}
		});
		pointers.count = new_count;
	}

	// now we construct the final result
	build_pointer_vector.count = result_count;
	// reference the columns of the left side from the result
	assert(left.column_count > 0);
	for (size_t i = 0; i < left.column_count; i++) {
		result.data[i].Reference(left.data[i]);
	}
	// now fetch the data from the RHS
	for (size_t i = 0; i < ht.build_types.size(); i++) {
		auto &vector = result.data[left.column_count + i];
		// set NULL entries for every entry that was not found
		vector.nullmask.set();
		for(size_t j = 0; j < result_count; j++) {
			vector.nullmask[result_sel_vector[j]] = false;
		}
		// fetch the data from the HT for tuples that found a match
		vector.sel_vector = result_sel_vector;
		vector.count = result_count;
		ht.build_serializer.DeserializeColumn(build_pointer_vector, i, vector);
		// now we fill in NULL values in the remaining entries 
		vector.count = result.size();
		vector.sel_vector = result.sel_vector;
	}
	// like the SEMI, ANTI and MARK join types, the SINGLE join only ever does one pass over the HT per input chunk
	finished = true;
}