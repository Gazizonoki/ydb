#include "defs.h"
#include "indexed_read_data.h"
#include "filter.h"
#include "column_engine_logs.h"
#include <ydb/core/tx/columnshard/columnshard__index_scan.h>
#include <ydb/core/tx/columnshard/columnshard__stats_scan.h>
#include <ydb/core/formats/one_batch_input_stream.h>
#include <ydb/core/formats/merging_sorted_input_stream.h>
#include <ydb/core/formats/custom_registry.h>

namespace NKikimr::NOlap {

namespace {

// Slices a batch into smaller batches and appends them to result vector (which might be non-empty already)
void SliceBatch(const std::shared_ptr<arrow::RecordBatch>& batch,
                const int64_t maxRowsInBatch,
                std::vector<std::shared_ptr<arrow::RecordBatch>>& result)
{
    if (batch->num_rows() <= maxRowsInBatch) {
        result.push_back(batch);
        return;
    }

    int64_t offset = 0;
    while (offset < batch->num_rows()) {
        int64_t rows = std::min<int64_t>(maxRowsInBatch, batch->num_rows() - offset);
        result.emplace_back(batch->Slice(offset, rows));
        offset += rows;
    }
};

std::vector<std::vector<std::shared_ptr<arrow::RecordBatch>>>
GroupInKeyRanges(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches, const TIndexInfo& indexInfo) {
    std::vector<std::vector<std::shared_ptr<arrow::RecordBatch>>> rangesSlices; // rangesSlices[rangeNo][sliceNo]
    rangesSlices.reserve(batches.size());
    {
        TMap<NArrow::TReplaceKey, std::vector<std::shared_ptr<arrow::RecordBatch>>> points;

        for (auto& batch : batches) {
            auto compositeKey = NArrow::ExtractColumns(batch, indexInfo.GetReplaceKey());
            Y_VERIFY(compositeKey && compositeKey->num_rows() > 0);
            auto keyColumns = std::make_shared<NArrow::TArrayVec>(compositeKey->columns());

            NArrow::TReplaceKey min(keyColumns, 0);
            NArrow::TReplaceKey max(keyColumns, compositeKey->num_rows() - 1);

            points[min].push_back(batch); // insert start
            points[max].push_back({}); // insert end
        }

        int sum = 0;
        for (auto& [key, vec] : points) {
            if (!sum) { // count(start) == count(end), start new range
                rangesSlices.push_back({});
                rangesSlices.back().reserve(batches.size());
            }

            for (auto& batch : vec) {
                if (batch) {
                    ++sum;
                    rangesSlices.back().push_back(batch);
                } else {
                    --sum;
                }
            }
        }
    }
    return rangesSlices;
}

std::vector<std::shared_ptr<arrow::RecordBatch>> SpecialMergeSorted(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                                                    const TIndexInfo& indexInfo,
                                                                    const std::shared_ptr<NArrow::TSortDescription>& description,
                                                                    const THashSet<const void*> batchesToDedup) {
    auto rangesSlices = GroupInKeyRanges(batches, indexInfo);

    // Merge slices in ranges
    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    out.reserve(rangesSlices.size());
    for (auto& slices : rangesSlices) {
        if (slices.empty()) {
            continue;
        }

        // Do not merge slice if it's alone in its key range
        if (slices.size() == 1) {
            auto batch = slices[0];
            if (batchesToDedup.count(batch.get())) {
                NArrow::DedupSortedBatch(batch, description->ReplaceKey, out);
            } else {
                Y_VERIFY_DEBUG(NArrow::IsSortedAndUnique(batch, description->ReplaceKey));
                out.push_back(batch);
            }
            continue;
        }
#if 0 // optimization
        auto deduped = SliceSortedBatches(slices, description);
        for (auto& batch : deduped) {
            if (batch && batch->num_rows()) {
                Y_VERIFY_DEBUG(NArrow::IsSortedAndUnique(batch, description->ReplaceKey));
                out.push_back(batch);
            }
        }
#else
        auto batch = NArrow::CombineSortedBatches(slices, description);
        out.push_back(batch);
#endif
    }

    return out;
}

}

std::unique_ptr<NColumnShard::TScanIteratorBase> TReadMetadata::StartScan(NColumnShard::TDataTasksProcessorContainer tasksProcessor, const NColumnShard::TScanCounters& scanCounters) const {
    return std::make_unique<NColumnShard::TColumnShardScanIterator>(this->shared_from_this(), tasksProcessor, scanCounters);
}

std::set<ui32> TReadMetadata::GetEarlyFilterColumnIds(const bool noTrivial) const {
    std::set<ui32> result;
    if (LessPredicate) {
        for (auto&& i : LessPredicate->ColumnNames()) {
            result.emplace(IndexInfo.GetColumnId(i));
            AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("early_filter_column", i);
        }
    }
    if (GreaterPredicate) {
        for (auto&& i : GreaterPredicate->ColumnNames()) {
            result.emplace(IndexInfo.GetColumnId(i));
            AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("early_filter_column", i);
        }
    }
    if (Program) {
        for (auto&& i : Program->GetEarlyFilterColumns()) {
            auto id = IndexInfo.GetColumnIdOptional(i);
            if (id) {
                result.emplace(*id);
                AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("early_filter_column", i);
            }
        }
    }
    if (noTrivial && result.empty()) {
        return result;
    }
    if (PlanStep) {
        auto snapSchema = TIndexInfo::ArrowSchemaSnapshot();
        for (auto&& i : snapSchema->fields()) {
            result.emplace(IndexInfo.GetColumnId(i->name()));
            AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("early_filter_column", i->name());
        }
    }
    return result;
}

std::set<ui32> TReadMetadata::GetUsedColumnIds() const {
    std::set<ui32> result;
    if (PlanStep) {
        auto snapSchema = TIndexInfo::ArrowSchemaSnapshot();
        for (auto&& i : snapSchema->fields()) {
            AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("used_column", i->name());
            result.emplace(IndexInfo.GetColumnId(i->name()));
        }
    }
    for (auto&& f : LoadSchema->fields()) {
        AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD_SCAN)("used_column", f->name());
        result.emplace(IndexInfo.GetColumnId(f->name()));
    }
    return result;
}

TVector<std::pair<TString, NScheme::TTypeInfo>> TReadStatsMetadata::GetResultYqlSchema() const {
    return NOlap::GetColumns(NColumnShard::PrimaryIndexStatsSchema, ResultColumnIds);
}

TVector<std::pair<TString, NScheme::TTypeInfo>> TReadStatsMetadata::GetKeyYqlSchema() const {
    return NOlap::GetColumns(NColumnShard::PrimaryIndexStatsSchema, NColumnShard::PrimaryIndexStatsSchema.KeyColumns);
}

std::unique_ptr<NColumnShard::TScanIteratorBase> TReadStatsMetadata::StartScan(NColumnShard::TDataTasksProcessorContainer /*tasksProcessor*/, const NColumnShard::TScanCounters& /*scanCounters*/) const {
    return std::make_unique<NColumnShard::TStatsIterator>(this->shared_from_this());
}

void TIndexedReadData::AddBlobForFetch(const TBlobRange& range, NIndexedReader::TBatch& batch) {
    Y_VERIFY(IndexedBlobs.emplace(range).second);
    Y_VERIFY(IndexedBlobSubscriber.emplace(range, &batch).second);
    if (batch.GetFilter()) {
        Counters.GetPostFilterBytes()->Add(range.Size);
        ReadMetadata->ReadStats->DataAdditionalBytes += range.Size;
        FetchBlobsQueue.emplace_front(range);
    } else {
        Counters.GetFilterBytes()->Add(range.Size);
        ReadMetadata->ReadStats->DataFilterBytes += range.Size;
        FetchBlobsQueue.emplace_back(range);
    }
}

void TIndexedReadData::InitRead(ui32 inputBatch, bool inGranulesOrder) {
    Y_VERIFY(ReadMetadata->BlobSchema);
    Y_VERIFY(ReadMetadata->LoadSchema);
    Y_VERIFY(ReadMetadata->ResultSchema);
    Y_VERIFY(IndexInfo().GetSortingKey());
    Y_VERIFY(IndexInfo().GetIndexKey() && IndexInfo().GetIndexKey()->num_fields());

    SortReplaceDescription = IndexInfo().SortReplaceDescription();

    NotIndexed.resize(inputBatch);

    ui32 batchNo = inputBatch;
    Batches.resize(inputBatch + ReadMetadata->SelectInfo->Portions.size(), nullptr);

    ui64 portionsBytes = 0;
    for (auto& portionInfo : ReadMetadata->SelectInfo->Portions) {
        portionsBytes += portionInfo.BlobsBytes();
        Y_VERIFY_S(portionInfo.Records.size() > 0, "ReadMeatadata: " << *ReadMetadata);

        ui64 granule = portionInfo.Records[0].Granule;

        auto itGranule = Granules.find(granule);
        if (itGranule == Granules.end()) {
            itGranule = Granules.emplace(granule, NIndexedReader::TGranule(granule, *this)).first;
        }

        NIndexedReader::TBatch& currentBatch = itGranule->second.AddBatch(batchNo, portionInfo);
        if (portionInfo.AllowEarlyFilter()) {
            currentBatch.Reset(&EarlyFilterColumns);
        } else {
            currentBatch.Reset(&UsedColumns);
        }
        Batches[batchNo] = &currentBatch;
        ++batchNo;
    }

    auto granulesOrder = ReadMetadata->SelectInfo->GranulesOrder(ReadMetadata->IsDescSorted());
    for (ui64 granule : granulesOrder) {
        auto it = Granules.find(granule);
        Y_VERIFY(it != Granules.end());
        if (inGranulesOrder) {
            GranulesOutOrder.emplace_back(&it->second);
        }
    }
    Counters.GetPortionBytes()->Add(portionsBytes);
    auto& stats = ReadMetadata->ReadStats;
    stats->IndexGranules = ReadMetadata->SelectInfo->Granules.size();
    stats->IndexPortions = ReadMetadata->SelectInfo->Portions.size();
    stats->IndexBatches = ReadMetadata->NumIndexedBlobs();
    stats->CommittedBatches = ReadMetadata->CommittedBlobs.size();
    stats->SchemaColumns = ReadMetadata->LoadSchema->num_fields();
    stats->FilterColumns = EarlyFilterColumns.size();
    stats->AdditionalColumns = PostFilterColumns.size();
    stats->PortionsBytes = portionsBytes;
}

void TIndexedReadData::AddIndexed(const TBlobRange& blobRange, const TString& data, NColumnShard::TDataTasksProcessorContainer processor) {
    NIndexedReader::TBatch* portionBatch = nullptr;
    {
        auto it = IndexedBlobSubscriber.find(blobRange);
        Y_VERIFY_DEBUG(it != IndexedBlobSubscriber.end());
        if (it == IndexedBlobSubscriber.end()) {
            return;
        }
        portionBatch = it->second;
        IndexedBlobSubscriber.erase(it);
    }
    if (!portionBatch->AddIndexedReady(blobRange, data)) {
        return;
    }
    if (portionBatch->IsFetchingReady()) {
        if (auto batch = portionBatch->AssembleTask(processor.GetObject(), ReadMetadata)) {
            processor.Add(*this, batch);
        }
    }
}

std::shared_ptr<arrow::RecordBatch>
TIndexedReadData::MakeNotIndexedBatch(const std::shared_ptr<arrow::RecordBatch>& srcBatch, ui64 planStep, ui64 txId) const {
    Y_VERIFY(srcBatch);

    // Extract columns (without check), filter, attach snapshot, extract columns with check
    // (do not filter snapshot columns)

    auto batch = NArrow::ExtractExistedColumns(srcBatch, ReadMetadata->LoadSchema);
    Y_VERIFY(batch);

    auto filter = FilterNotIndexed(batch, *ReadMetadata);
    if (filter.IsTotalDenyFilter()) {
        return nullptr;
    }
    auto preparedBatch = batch;

    preparedBatch = TIndexInfo::AddSpecialColumns(preparedBatch, planStep, txId);
    Y_VERIFY(preparedBatch);

    preparedBatch = NArrow::ExtractColumns(preparedBatch, ReadMetadata->LoadSchema);
    Y_VERIFY(preparedBatch);

    filter.Apply(preparedBatch);
    return preparedBatch;
}

TVector<TPartialReadResult> TIndexedReadData::GetReadyResults(const int64_t maxRowsInBatch) {
    Y_VERIFY(SortReplaceDescription);

    if (NotIndexed.size() != ReadyNotIndexed) {
        // Wait till we have all not indexed data so we could replace keys in granules
        return {};
    }

    // First time extract OutNotIndexed data
    if (NotIndexed.size()) {
        auto mergedBatch = MergeNotIndexed(std::move(NotIndexed)); // merged has no dups
        if (mergedBatch) {
            // Init split by granules structs
            Y_VERIFY(ReadMetadata->SelectInfo);
            TColumnEngineForLogs::TMarksGranules marksGranules(*ReadMetadata->SelectInfo);

            // committed data before the first granule would be placed in fake preceding granule
            // committed data after the last granule would be placed into the last granule
            marksGranules.MakePrecedingMark(IndexInfo());
            Y_VERIFY(!marksGranules.Empty());

            OutNotIndexed = marksGranules.SliceIntoGranules(mergedBatch, IndexInfo());
        }
        NotIndexed.clear();
        ReadyNotIndexed = 0;
    }

    // Extract ready to out granules: ready granules that are not blocked by other (not ready) granules
    const bool requireResult = !IsInProgress(); // not indexed or the last indexed read (even if it's empty)
    auto out = MakeResult(ReadyToOut(), maxRowsInBatch);
    if (requireResult && out.empty()) {
        out.push_back(TPartialReadResult{
            .ResultBatch = NArrow::MakeEmptyBatch(ReadMetadata->ResultSchema)
        });
    }
    return out;
}

std::vector<NIndexedReader::TGranule*> TIndexedReadData::DetachReadyInOrder() {
    std::vector<NIndexedReader::TGranule*> out;
    out.reserve(GranulesToOut.size());

    if (GranulesOutOrder.empty()) {
        for (auto& [_, granule] : GranulesToOut) {
            out.emplace_back(granule);
        }
        GranulesToOut.clear();
    } else {
        while (GranulesOutOrder.size()) {
            NIndexedReader::TGranule* granule = GranulesOutOrder.front();
            if (!granule->IsReady()) {
                break;
            }
            out.emplace_back(granule);
            Y_VERIFY(GranulesToOut.erase(granule->GetGranuleId()));
            GranulesOutOrder.pop_front();
        }
    }

    return out;
}

/// @return batches that are not blocked by others
std::vector<std::vector<std::shared_ptr<arrow::RecordBatch>>> TIndexedReadData::ReadyToOut() {
    Y_VERIFY(SortReplaceDescription);

    std::vector<std::vector<std::shared_ptr<arrow::RecordBatch>>> out;
    out.reserve(GranulesToOut.size() + 1);

    // Prepend not indexed data (less then first granule) before granules for ASC sorting
    if (ReadMetadata->IsAscSorted() && OutNotIndexed.count(0)) {
        out.push_back({});
        out.back().push_back(OutNotIndexed[0]);
        OutNotIndexed.erase(0);
    }

    std::vector<NIndexedReader::TGranule*> ready = DetachReadyInOrder();
    for (auto&& granule : ready) {
        bool canHaveDups = granule->IsDuplicationsAvailable();
        std::vector<std::shared_ptr<arrow::RecordBatch>> inGranule = granule->GetReadyBatches();
        // Append not indexed data to granules
        if (OutNotIndexed.count(granule->GetGranuleId())) {
            auto batch = OutNotIndexed[granule->GetGranuleId()];
            if (batch && batch->num_rows()) { // TODO: check why it could be empty
                inGranule.push_back(batch);
                canHaveDups = true;
            }
            OutNotIndexed.erase(granule->GetGranuleId());
        }

        if (inGranule.empty()) {
            continue;
        }

        if (canHaveDups) {
            for (auto& batch : inGranule) {
                Y_VERIFY(batch->num_rows());
                Y_VERIFY_DEBUG(NArrow::IsSorted(batch, SortReplaceDescription->ReplaceKey));
            }
#if 1 // optimization
            auto deduped = SpecialMergeSorted(inGranule, IndexInfo(), SortReplaceDescription, BatchesToDedup);
            out.emplace_back(std::move(deduped));
#else
            out.push_back({});
            out.back().emplace_back(CombineSortedBatches(inGranule, SortReplaceDescription));
#endif
        } else {
            out.emplace_back(std::move(inGranule));
        }
    }

    // Append not indexed data (less then first granule) after granules for DESC sorting
    if (ReadMetadata->IsDescSorted() && GranulesOutOrder.empty() && OutNotIndexed.count(0)) {
        out.push_back({});
        out.back().push_back(OutNotIndexed[0]);
        OutNotIndexed.erase(0);
    }

    return out;
}

std::shared_ptr<arrow::RecordBatch>
TIndexedReadData::MergeNotIndexed(std::vector<std::shared_ptr<arrow::RecordBatch>>&& batches) const {
    Y_VERIFY(ReadMetadata->IsSorted());
    Y_VERIFY(IndexInfo().GetSortingKey());

    { // remove empty batches
        size_t dst = 0;
        for (size_t src = 0; src < batches.size(); ++src) {
            if (batches[src] && batches[src]->num_rows()) {
                if (dst != src) {
                    batches[dst] = batches[src];
                }
                ++dst;
            }
        }
        batches.resize(dst);
    }

    if (batches.empty()) {
        return {};
    }

    // We could merge data here only if backpressure limits committed data size. KIKIMR-12520
    auto& indexInfo = IndexInfo();
    auto merged = NArrow::CombineSortedBatches(batches, indexInfo.SortReplaceDescription());
    Y_VERIFY(merged);
    Y_VERIFY_DEBUG(NArrow::IsSortedAndUnique(merged, indexInfo.GetReplaceKey()));
    return merged;
}

// TODO: better implementation
static void MergeTooSmallBatches(TVector<TPartialReadResult>& out) {
    if (out.size() < 10) {
        return;
    }

    i64 sumRows = 0;
    for (auto& result : out) {
        sumRows += result.ResultBatch->num_rows();
    }
    if (sumRows / out.size() > 100) {
        return;
    }

    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    batches.reserve(out.size());
    for (auto& batch : out) {
        batches.push_back(batch.ResultBatch);
    }

    auto res = arrow::Table::FromRecordBatches(batches);
    if (!res.ok()) {
        Y_VERIFY_DEBUG(false, "Cannot make table from batches");
        return;
    }

    res = (*res)->CombineChunks();
    if (!res.ok()) {
        Y_VERIFY_DEBUG(false, "Cannot combine chunks");
        return;
    }
    auto batch = NArrow::ToBatch(*res);

    TVector<TPartialReadResult> merged;
    merged.emplace_back(TPartialReadResult{
        .ResultBatch = std::move(batch),
        .LastReadKey = std::move(out.back().LastReadKey)
    });
    out.swap(merged);
}

TVector<TPartialReadResult>
TIndexedReadData::MakeResult(std::vector<std::vector<std::shared_ptr<arrow::RecordBatch>>>&& granules,
                             int64_t maxRowsInBatch) const {
    Y_VERIFY(ReadMetadata->IsSorted());
    Y_VERIFY(SortReplaceDescription);

    TVector<TPartialReadResult> out;

    bool isDesc = ReadMetadata->IsDescSorted();

    for (auto& batches : granules) {
        if (batches.empty()) {
            continue;
        }

        {
            std::vector<std::shared_ptr<arrow::RecordBatch>> splitted;
            splitted.reserve(batches.size());
            for (auto& batch : batches) {
                SliceBatch(batch, maxRowsInBatch, splitted);
            }
            batches.swap(splitted);
        }

        if (isDesc) {
            std::vector<std::shared_ptr<arrow::RecordBatch>> reversed;
            reversed.reserve(batches.size());
            for (int i = batches.size() - 1; i >= 0; --i) {
                auto& batch = batches[i];
                auto permutation = NArrow::MakePermutation(batch->num_rows(), true);
                auto res = arrow::compute::Take(batch, permutation);
                Y_VERIFY(res.ok());
                reversed.push_back((*res).record_batch());
            }
            batches.swap(reversed);
        }

        for (auto& batch : batches) {
            Y_VERIFY_DEBUG(NArrow::IsSortedAndUnique(batch, IndexInfo().GetReplaceKey(), isDesc));

            if (batch->num_rows() == 0) {
                Y_VERIFY_DEBUG(false, "Unexpected empty batch");
                continue;
            }

            // Extract the last row's PK
            auto keyBatch = NArrow::ExtractColumns(batch, IndexInfo().GetReplaceKey());
            auto lastKey = keyBatch->Slice(keyBatch->num_rows()-1, 1);

            // Leave only requested columns
            auto resultBatch = NArrow::ExtractColumns(batch, ReadMetadata->ResultSchema);
            out.emplace_back(TPartialReadResult{
                .ResultBatch = std::move(resultBatch),
                .LastReadKey = std::move(lastKey)
            });
        }
    }

    if (ReadMetadata->Program) {
        MergeTooSmallBatches(out);

        for (auto& result : out) {
            auto status = ApplyProgram(result.ResultBatch, *ReadMetadata->Program, NArrow::GetCustomExecContext());
            if (!status.ok()) {
                result.ErrorString = status.message();
            }
        }
    }
    return out;
}

NIndexedReader::TBatch& TIndexedReadData::GetBatchInfo(const ui32 batchNo) {
    Y_VERIFY(batchNo < Batches.size());
    auto ptr = Batches[batchNo];
    Y_VERIFY(ptr);
    return *ptr;
}

TIndexedReadData::TIndexedReadData(NOlap::TReadMetadata::TConstPtr readMetadata, TFetchBlobsQueue& fetchBlobsQueue,
    const bool internalRead, const NColumnShard::TScanCounters& counters)
    : Counters(counters)
    , FetchBlobsQueue(fetchBlobsQueue)
    , ReadMetadata(readMetadata)
{
    UsedColumns = ReadMetadata->GetUsedColumnIds();
    PostFilterColumns = ReadMetadata->GetUsedColumnIds();
    EarlyFilterColumns = ReadMetadata->GetEarlyFilterColumnIds(true);
    if (internalRead || EarlyFilterColumns.empty()) {
        EarlyFilterColumns = PostFilterColumns;
        PostFilterColumns.clear();
    } else {
        for (auto&& i : EarlyFilterColumns) {
            PostFilterColumns.erase(i);
        }
    }
    Y_VERIFY(ReadMetadata->SelectInfo);
}

}
