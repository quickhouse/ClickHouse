#include <Processors/Transforms/DistinctSortedChunkTransform.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int SET_SIZE_LIMIT_EXCEEDED;
}

DistinctSortedChunkTransform::DistinctSortedChunkTransform(
    const Block & header_,
    const SizeLimits & output_size_limits_,
    UInt64 limit_hint_,
    const SortDescription & sorted_columns_descr_,
    const Names & source_columns)
    : ISimpleTransform(header_, header_, true)
    , limit_hint(limit_hint_)
    , output_size_limits(output_size_limits_)
    , sorted_columns_descr(sorted_columns_descr_)
{
    /// calculate sorted columns positions
    sorted_columns_pos.reserve(sorted_columns_descr.size());
    for (auto const & descr : sorted_columns_descr)
    {
        size_t pos = header_.getPositionByName(descr.column_name);
        sorted_columns_pos.emplace_back(pos);
    }

    /// calculate non-sorted columns positions
    other_columns_pos.reserve(source_columns.size());
    for (const auto & source_column : source_columns)
    {
        size_t pos = header_.getPositionByName(source_column);
        if (std::find(sorted_columns_pos.begin(), sorted_columns_pos.end(), pos) != sorted_columns_pos.end())
            continue;

        const auto & col = header_.getByPosition(pos).column;
        if (col && !isColumnConst(*col))
            other_columns_pos.emplace_back(pos);
    }

    /// reserve space in auxiliary column vectors for processing
    sorted_columns.reserve(sorted_columns_pos.size());
    other_columns.reserve(other_columns_pos.size());
    current_key.reserve(sorted_columns.size());
}

void DistinctSortedChunkTransform::initChunkProcessing(const Columns & input_columns)
{
    sorted_columns.clear();
    for (size_t pos : sorted_columns_pos)
        sorted_columns.emplace_back(input_columns[pos].get());

    other_columns.clear();
    for (size_t pos : other_columns_pos)
        other_columns.emplace_back(input_columns[pos].get());

    if (!other_columns.empty() && data.type == ClearableSetVariants::Type::EMPTY)
        data.init(ClearableSetVariants::chooseMethod(other_columns, other_columns_sizes));
}

size_t DistinctSortedChunkTransform::ordinaryDistinctOnRange(IColumn::Filter & filter, size_t range_begin, size_t range_end, bool clear_data)
{
    size_t count = 0;
    switch (data.type)
    {
        case ClearableSetVariants::Type::EMPTY:
            break;
            // clang-format off
#define M(NAME) \
        case ClearableSetVariants::Type::NAME: \
            count = buildFilterForRange(*data.NAME, filter, range_begin, range_end, clear_data); \
            break;

        APPLY_FOR_SET_VARIANTS(M)
#undef M
            // clang-format on
    }
    return count;
}

template <typename Method>
size_t DistinctSortedChunkTransform::buildFilterForRange(
    Method & method, IColumn::Filter & filter, size_t range_begin, size_t range_end, bool clear_data)
{
    typename Method::State state(other_columns, other_columns_sizes, nullptr);
    if (clear_data)
        method.data.clear();

    size_t count = 0;
    for (size_t i = range_begin; i < range_end; ++i)
    {
        auto emplace_result = state.emplaceKey(method.data, i, data.string_pool);

        /// emit the record if there is no such key in the current set, skip otherwise
        filter[i] = emplace_result.isInserted();
        if (filter[i])
            ++count;
    }
    return count;
}

void DistinctSortedChunkTransform::setCurrentKey(const size_t row_pos)
{
    current_key.clear();
    for (auto const & col : sorted_columns)
    {
        current_key.emplace_back(col->cloneEmpty());
        current_key.back()->insertFrom(*col, row_pos);
    }
}

bool DistinctSortedChunkTransform::isCurrentKey(const size_t row_pos) const
{
    for (size_t i = 0; i < sorted_columns.size(); ++i)
    {
        int res = current_key[i]->compareAt(0, row_pos, *sorted_columns[i], sorted_columns_descr[i].nulls_direction);
        if (res != 0)
            return false;
    }
    return true;
}

size_t DistinctSortedChunkTransform::getRangeEnd(size_t begin, size_t end) const
{
    assert(begin < end);

    const size_t linear_probe_threadhold = 16;
    size_t linear_probe_end = begin + linear_probe_threadhold;
    if (linear_probe_end > end)
        linear_probe_end = end;

    for (size_t pos = begin; pos < linear_probe_end; ++pos)
    {
        if (!isCurrentKey(pos))
            return pos;
    }

    size_t low = linear_probe_end;
    size_t high = end - 1;
    while (low <= high)
    {
        size_t mid = low + (high - low) / 2;
        if (isCurrentKey(mid))
            low = mid + 1;
        else
        {
            high = mid - 1;
            end = mid;
        }
    }
    return end;
}

std::pair<size_t, size_t> DistinctSortedChunkTransform::continueWithPrevRange(const size_t chunk_rows, IColumn::Filter & filter)
{
    /// current_key is empty on very first transform() call
    /// or first row doesn't match a key from previous transform()
    if (current_key.empty() || !isCurrentKey(0))
        return {0, 0};

    size_t output_rows = 0;
    const size_t range_end = getRangeEnd(0, chunk_rows);
    if (other_columns.empty())
        std::fill(filter.begin(), filter.begin() + range_end, 0); /// skip rows already included in distinct on previous transform()
    else
        output_rows = ordinaryDistinctOnRange(filter, 0, range_end, false);

    return {range_end, output_rows};
}

void DistinctSortedChunkTransform::transform(Chunk & chunk)
{
    const size_t chunk_rows = chunk.getNumRows();
    if (unlikely(0 == chunk_rows))
        return;

    Columns input_columns = chunk.detachColumns();
    /// split input columns into sorted and other("non-sorted") columns
    initChunkProcessing(input_columns);

    /// build filter:
    /// (1) find range with the same values in sorted columns -> [range_begin, range_end)
    /// (2) for found range
    ///     if there is no "non-sorted" columns: filter out all rows in range except first one
    ///     otherwise: apply ordinary distinct
    /// (3) repeat until chunk is processed
    IColumn::Filter filter(chunk_rows);
    auto [range_begin, output_rows] = continueWithPrevRange(chunk_rows, filter); /// try to process chuck as continuation of previous one
    size_t range_end = range_begin;
    while (range_end != chunk_rows)
    {
        // set current key to find range
        setCurrentKey(range_begin);

        // find new range [range_begin, range_end)
        range_end = getRangeEnd(range_begin, chunk_rows);

        // update filter for range
        if (other_columns.empty())
        {
            filter[range_begin] = 1;
            std::fill(filter.begin() + range_begin + 1, filter.begin() + range_end, 0);
            ++output_rows;
        }
        else
        {
            // ordinary distinct in range if there are "non-sorted" columns
            output_rows += ordinaryDistinctOnRange(filter, range_begin, range_end, true);
        }

        // set where next range start
        range_begin = range_end;
    }

    /// apply the built filter
    for (auto & input_column : input_columns)
        input_column = input_column->filter(filter, output_rows);

    chunk.setColumns(std::move(input_columns), output_rows);

    /// Update total output rows and check limits
    total_output_rows += output_rows;
    if ((limit_hint && total_output_rows >= limit_hint)
        || !output_size_limits.check(total_output_rows, data.getTotalByteCount(), "DISTINCT", ErrorCodes::SET_SIZE_LIMIT_EXCEEDED))
    {
        stopReading();
    }
}

}
