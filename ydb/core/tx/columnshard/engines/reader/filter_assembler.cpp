#include "filter_assembler.h"
#include <ydb/core/tx/columnshard/engines/filter.h>

namespace NKikimr::NOlap::NIndexedReader {

bool TAssembleFilter::DoExecuteImpl() {
    /// @warning The replace logic is correct only in assumption that predicate is applied over a part of ReplaceKey.
    /// It's not OK to apply predicate before replacing key duplicates otherwise.
    /// Assumption: dup(A, B) <=> PK(A) = PK(B) => Predicate(A) = Predicate(B) => all or no dups for PK(A) here
    auto batch = BatchConstructor.Assemble();
    Y_VERIFY(batch);
    Y_VERIFY(batch->num_rows());
    OriginalCount = batch->num_rows();
    Filter = std::make_shared<NArrow::TColumnFilter>(NOlap::FilterPortion(batch, *ReadMetadata));
    if (!Filter->Apply(batch)) {
        AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("event", "skip_data")("original_count", OriginalCount);
        FilteredBatch = nullptr;
        return true;
    }
    if (ReadMetadata->Program && AllowEarlyFilter) {
        auto filter = NOlap::EarlyFilter(batch, ReadMetadata->Program);
        Filter->CombineSequential(filter);
        if (!filter.Apply(batch)) {
            AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("event", "skip_data")("original_count", OriginalCount);
            FilteredBatch = nullptr;
            return true;
        }
    }
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("event", "not_skip_data")
        ("original_count", OriginalCount)("filtered_count", batch->num_rows())("columns_count", BatchConstructor.GetColumnsCount())("allow_early", AllowEarlyFilter);

    FilteredBatch = batch;
    return true;
}

bool TAssembleFilter::DoApply(TIndexedReadData& owner) const {
    TBatch& batch = owner.GetBatchInfo(BatchNo);
    Y_VERIFY(OriginalCount);
    owner.GetCounters().GetOriginalRowsCount()->Add(OriginalCount);
    batch.InitFilter(Filter, FilteredBatch);
    owner.GetCounters().GetAssembleFilterCount()->Add(1);
    if (!FilteredBatch || FilteredBatch->num_rows() == 0) {
        owner.GetCounters().GetEmptyFilterCount()->Add(1);
        owner.GetCounters().GetEmptyFilterFetchedBytes()->Add(batch.GetFetchedBytes());
        owner.GetCounters().GetSkippedBytes()->Add(batch.GetFetchBytes(&owner.GetPostFilterColumns()));
        batch.InitBatch(FilteredBatch);
    } else {
        owner.GetCounters().GetFilteredRowsCount()->Add(FilteredBatch->num_rows());
        if (batch.AskedColumnsAlready(owner.GetPostFilterColumns())) {
            owner.GetCounters().GetFilterOnlyCount()->Add(1);
            owner.GetCounters().GetFilterOnlyFetchedBytes()->Add(batch.GetFetchedBytes());
            owner.GetCounters().GetFilterOnlyUsefulBytes()->Add(batch.GetFetchedBytes() * FilteredBatch->num_rows() / OriginalCount);
            owner.GetCounters().GetSkippedBytes()->Add(batch.GetFetchBytes(&owner.GetPostFilterColumns()));

            batch.InitBatch(FilteredBatch);
        } else {
            owner.GetCounters().GetTwoPhasesFilterFetchedBytes()->Add(batch.GetFetchedBytes());
            owner.GetCounters().GetTwoPhasesFilterUsefulBytes()->Add(batch.GetFetchedBytes() * FilteredBatch->num_rows() / OriginalCount);

            batch.Reset(&owner.GetPostFilterColumns());

            owner.GetCounters().GetTwoPhasesCount()->Add(1);
            owner.GetCounters().GetTwoPhasesPostFilterFetchedBytes()->Add(batch.GetWaitingBytes());
            owner.GetCounters().GetTwoPhasesPostFilterUsefulBytes()->Add(batch.GetWaitingBytes() * FilteredBatch->num_rows() / OriginalCount);
            AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("event", "additional_data")
                ("filtered_count", FilteredBatch->num_rows())
                ("blobs_count", batch.GetWaitingBlobs().size())
                ("columns_count", batch.GetCurrentColumnIds()->size())
                ("fetch_size", batch.GetWaitingBytes())
                ;
        }
    }
    return true;
}

}
