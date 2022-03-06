"""
DuckDB data chunk
"""
mutable struct DataChunk
    handle::duckdb_data_chunk

    function DataChunk(handle::duckdb_data_chunk)
        result = new(handle)
        return result
    end
end

function GetSize(chunk::DataChunk)
    return duckdb_data_chunk_get_size(chunk.handle)
end

function SetSize(chunk::DataChunk, size::Int64)
    return duckdb_data_chunk_set_size(chunk.handle, size)
end

function GetArray(chunk::DataChunk, col_idx::Int64, ::Type{T})::Vector{T} where {T}
    raw_ptr = duckdb_data_chunk_get_data(chunk.handle, col_idx)
    ptr = Base.unsafe_convert(Ptr{T}, raw_ptr)
    return unsafe_wrap(Vector{T}, ptr, VECTOR_SIZE, own = false)
end

function GetValidity(chunk::DataChunk, col_idx::Int64)::ValidityMask
    duckdb_data_chunk_ensure_validity_writable(chunk.handle, col_idx)
    validity_ptr = duckdb_data_chunk_get_validity(chunk.handle, col_idx)
    ptr = Base.unsafe_convert(Ptr{UInt64}, validity_ptr)
    validity_vector = unsafe_wrap(Vector{UInt64}, ptr, VECTOR_SIZE ÷ BITS_PER_VALUE, own = false)
    return ValidityMask(validity_vector)
end

# this is only required when we own the data chunk
# function _destroy_data_chunk(chunk::DataChunk)
#     if chunk.handle != C_NULL
#         duckdb_destroy_data_chunk(chunk.handle)
#     end
#     chunk.handle = C_NULL
# end
