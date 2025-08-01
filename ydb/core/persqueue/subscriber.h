#pragma once

#include "blob.h"
#include "header.h"
#include "partition_id.h"
#include "utils.h"

#include <ydb/core/tablet/tablet_counters.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/persqueue/events/internal.h>
#include <ydb/core/persqueue/blob_refcounter.h>

namespace NKikimr {
namespace NPQ {

struct TUserInfo;

struct TReadAnswer {
    ui64 Size = 0;
    THolder<IEventBase> Event;
};

struct TReadInfo {
    TString User;
    TString ClientDC;
    ui64 Offset;
    ui16 PartNo;
    ui32 Count;
    ui32 Size;
    ui64 Destination;
    TInstant Timestamp;
    ui64 ReadTimestampMs;
    TDuration WaitQuotaTime;
    bool IsExternalRead;

    bool IsSubscription;

    TVector<TRequestedBlob> Blobs; //offset, count, value
    ui64 CachedOffset; //offset of head can be bigger than last databody offset
    TVector<TClientBlob> Cached; //records from head
    TActorId PipeClient;

    ui64 SizeEstimate = 0;
    ui64 RealReadOffset = 0;
    ui64 LastOffset = 0;
    bool Error = false;

    TBlobKeyTokens BlobKeyTokens;
    size_t CompactedBlobsCount = 0;

    TReadInfo() = delete;
    TReadInfo(
        const TString& user,
        const TString& clientDC,
        const ui64 offset,
        const ui64 lastOffset,
        const ui16 partNo,
        const ui64 count,
        const ui32 size,
        const ui64 dst,
        ui64 readTimestampMs,
        TDuration waitQuotaTime,
        const bool isExternalRead,
        const TActorId& pipeClient
    )
        : User(user)
        , ClientDC(clientDC)
        , Offset(offset)
        , PartNo(partNo)
        , Count(count)
        , Size(size)
        , Destination(dst)
        , Timestamp(TAppData::TimeProvider->Now())
        , ReadTimestampMs(readTimestampMs)
        , WaitQuotaTime(waitQuotaTime)
        , IsExternalRead(isExternalRead)
        , IsSubscription(false)
        , CachedOffset(0)
        , PipeClient(pipeClient)
        , LastOffset(lastOffset)
    {}

    TReadAnswer FormAnswer(
        const TActorContext& ctx,
        const TEvPQ::TEvBlobResponse& response,
        const ui64 startOffset,
        const ui64 endOffset,
        const TPartitionId& partition,
        TUserInfo* ui,
        const ui64 dst,
        const ui64 sizeLag,
        const TActorId& tablet,
        const NKikimrPQ::TPQTabletConfig::EMeteringMode meteringMode,
        const bool isActive,
        const std::function<void(bool readingFinished, NKikimrClient::TCmdReadResult& r)>& postProcessor
    );

    TReadAnswer FormAnswer(
        const TActorContext& ctx,
        const TEvPQ::TEvBlobResponse* response,
        const ui64 startOffset,
        const ui64 endOffset,
        const TPartitionId& partition,
        TUserInfo* ui,
        const ui64 dst,
        const ui64 sizeLag,
        const TActorId& tablet,
        const NKikimrPQ::TPQTabletConfig::EMeteringMode meteringMode,
        const bool isActive,
        const std::function<void(bool readingFinished, NKikimrClient::TCmdReadResult& r)>& postProcessor
    ) {
        static TEvPQ::TEvBlobResponse fakeBlobResponse(0, TVector<TRequestedBlob>());
        if (!response) {
            response = &fakeBlobResponse;
        }
        return FormAnswer(ctx, *response, startOffset, endOffset, partition, ui, dst, sizeLag, tablet, meteringMode, isActive, postProcessor);
    }

    bool UpdateUsage(const TClientBlob& blob,
                     ui32& cnt, ui32& size, ui32& lastBlobSize) const;
    TMaybe<TReadAnswer> AddBlobsFromBody(const TVector<NPQ::TRequestedBlob>& blobs,
                                         const ui32 begin, const ui32 end,
                                         TUserInfo* userInfo,
                                         const ui64 startOffset,
                                         const ui64 endOffset,
                                         const ui64 sizeLag,
                                         const TActorId& tablet,
                                         ui64 realReadOffset,
                                         NKikimrClient::TCmdReadResult* readResult,
                                         THolder<TEvPQ::TEvProxyResponse>& answer,
                                         bool& needStop,
                                         ui32& cnt, ui32& size, ui32& lastBlobSize,
                                         const TActorContext& ctx);
};

struct TOffsetCookie {
    ui64 Offset;
    ui64 Cookie;
};


class TSubscriberLogic : public TNonCopyable {
public:
    TSubscriberLogic();

    //will store and return cookie
    void AddSubscription(TReadInfo&& info, const ui64 cookie);

    //forget on timeout
    TMaybe<TReadInfo> ForgetSubscription(const ui64 cookie);

    //form TReadInfo::Cached with new data and return ready reads
    TVector<std::pair<TReadInfo, ui64>> CompleteSubscriptions(const ui64 endOffset);
private:
    THashMap<ui64, TReadInfo> ReadInfo;    // cookie -> {...}
    std::deque<TOffsetCookie> WaitingReads;
};


class TSubscriber : public TNonCopyable {
public:
    TSubscriber(const TPartitionId& partition, TTabletCountersBase& counters, const TActorId& tablet);

    //will wait for new data or timeout for this read and set timer for timeout ms
    void AddSubscription(TReadInfo&& info, const ui32 timeout, const ui64 cookie, const TActorContext& ctx);

    //handle of timeout for some read
    TMaybe<TReadInfo> OnTimeout(TEvPQ::TEvReadTimeout::TPtr& ev);

    //get completed subscriptions
    TVector<std::pair<TReadInfo, ui64>> GetReads(const ui64 endOffsets);

private:
    TSubscriberLogic Subscriber;
    const TPartitionId Partition;
    TTabletCountersBase& Counters;
    TActorId Tablet;
};




}// NPQ
}// NKikimr
