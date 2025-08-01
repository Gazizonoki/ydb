#include "agent_impl.h"

namespace NKikimr::NBlobDepot {

    template<>
    TBlobDepotAgent::TQuery *TBlobDepotAgent::CreateQuery<TEvBlobStorage::EvPut>(std::unique_ptr<IEventHandle> ev,
            TMonotonic received) {
        class TPutQuery : public TBlobStorageQuery<TEvBlobStorage::TEvPut> {
            const bool SuppressFooter = true;
            const bool IssueUncertainWrites = false;

            ui32 PutsInFlight = 0;
            bool PutsIssued = false;
            bool WaitingForCommitBlobSeq = false;
            bool IsInFlight = false;
            bool WrittenBeyondBarrier = false;
            NKikimrBlobDepot::TEvCommitBlobSeq CommitBlobSeq;
            TBlobSeqId BlobSeqId;
            std::optional<TS3Locator> LocatorInFlight;
            TActorId WriterActorId;
            ui64 ConnectionInstanceOnStart;

        public:
            using TBlobStorageQuery::TBlobStorageQuery;

            void OnDestroy(bool success) override {
                if (IsInFlight || LocatorInFlight) {
                    Y_ABORT_UNLESS(!success);
                    if (IsInFlight) {
                        RemoveBlobSeqFromInFlight();
                    }
                    NKikimrBlobDepot::TEvDiscardSpoiledBlobSeq msg;
                    if (IsInFlight) {
                        BlobSeqId.ToProto(msg.AddItems());
                    }
                    if (LocatorInFlight) {
                        LocatorInFlight->ToProto(msg.AddS3Locators());
                    }
                    Agent.Issue(std::move(msg), this, nullptr);
                }

                if (WriterActorId) {
                    TActivationContext::Send(new IEventHandle(TEvents::TSystem::Poison, 0, WriterActorId, Agent.SelfId(),
                        nullptr, 0));
                }

                TBlobStorageQuery::OnDestroy(success);
            }

            void Initiate() override {
                if (Request.Buffer.size() > MaxBlobSize) {
                    return EndWithError(NKikimrProto::ERROR, "blob is way too big");
                } else if (Request.Buffer.size() != Request.Id.BlobSize()) {
                    return EndWithError(NKikimrProto::ERROR, "blob size mismatch");
                } else if (!Request.Buffer) {
                    return EndWithError(NKikimrProto::ERROR, "no blob data");
                } else if (!Request.Id) {
                    return EndWithError(NKikimrProto::ERROR, "blob id is zero");
                }

                BlockChecksRemain = (1 + Request.ExtraBlockChecks.size()) * 3; // set number of tries for every block
                CheckBlocks();
            }

            void CheckBlocks() {
                bool someBlocksMissing = false;
                for (size_t i = Request.IgnoreBlock; i <= Request.ExtraBlockChecks.size(); ++i) {
                    const auto *blkp = i ? &Request.ExtraBlockChecks[i - 1] : nullptr;
                    const ui64 tabletId = blkp ? blkp->first : Request.Id.TabletID();
                    const ui32 generation = blkp ? blkp->second : Request.Id.Generation();
                    const auto status = CheckBlockForTablet(tabletId, generation);
                    if (status == NKikimrProto::OK) {
                        continue;
                    } else {
                        someBlocksMissing = true;
                    }
                }
                if (!someBlocksMissing) {
                    IssuePuts();
                }
            }

            void IssuePuts() {
                Y_ABORT_UNLESS(!PutsIssued);

                auto prepare = [&] {
                    Y_ABORT_UNLESS(CommitBlobSeq.ItemsSize() == 0);
                    auto *commitItem = CommitBlobSeq.AddItems();
                    commitItem->SetKey(Request.Id.AsBinaryString());
                    if (Request.IssueKeepFlag) {
                        commitItem->SetIssueKeepFlag(true);
                    }
                    if (Request.IgnoreBlock) {
                        commitItem->SetIgnoreBlock(true);
                    }
                    for (const auto& [tabletId, generation] : Request.ExtraBlockChecks) {
                        auto *p = commitItem->AddExtraBlockChecks();
                        p->SetTabletId(tabletId);
                        p->SetGeneration(generation);
                    }
                    return commitItem;
                };

                if (const auto& s3 = Agent.S3BackendSettings; s3 && s3->HasSyncMode()) {
                    auto *commitItem = prepare();
                    commitItem->MutableS3Locator();
                    return IssueS3Put();
                }

                const auto it = Agent.ChannelKinds.find(NKikimrBlobDepot::TChannelKind::Data);
                if (it == Agent.ChannelKinds.end()) {
                    return EndWithError(NKikimrProto::ERROR, "no Data channels");
                }
                auto& kind = it->second;

                std::optional<TBlobSeqId> blobSeqId = kind.Allocate(Agent);
                STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA21, "allocated BlobSeqId", (AgentId, Agent.LogId),
                    (QueryId, GetQueryId()), (BlobSeqId, blobSeqId), (BlobId, Request.Id));
                if (!blobSeqId) {
                    return kind.EnqueueQueryWaitingForId(this);
                }
                BlobSeqId = *blobSeqId;
                if (!IssueUncertainWrites) {
                    // small optimization -- do not put this into WritesInFlight as it will be deleted right after in
                    // this function
                    kind.WritesInFlight.insert(BlobSeqId);
                    IsInFlight = true;
                }

                BDEV_QUERY(BDEV09, "TEvPut_new", (U.BlobId, Request.Id), (U.BufferSize, Request.Buffer.size()),
                    (U.HandleClass, Request.HandleClass));

                auto *commitItem = prepare();
                auto *locator = commitItem->MutableBlobLocator();
                BlobSeqId.ToProto(locator->MutableBlobSeqId());
                //locator->SetChecksum(Crc32c(Request.Buffer.data(), Request.Buffer.size()));
                locator->SetTotalDataLen(Request.Buffer.size());
                if (!SuppressFooter) {
                    locator->SetFooterLen(sizeof(TVirtualGroupBlobFooter));
                }

                TRcBuf footerData;
                if (!SuppressFooter) {
                    footerData = TRcBuf::Uninitialized(sizeof(TVirtualGroupBlobFooter));
                    auto& footer = *reinterpret_cast<TVirtualGroupBlobFooter*>(footerData.UnsafeGetDataMut());
                    memset(&footer, 0, sizeof(footer));
                    footer.StoredBlobId = Request.Id;
                }

                auto put = [&](EBlobType type, TRcBuf&& buffer) {
                    const auto& [id, groupId] = kind.MakeBlobId(Agent, BlobSeqId, type, 0, buffer.size());
                    Y_ABORT_UNLESS(!locator->HasGroupId() || locator->GetGroupId() == groupId);
                    locator->SetGroupId(groupId);
                    auto ev = std::make_unique<TEvBlobStorage::TEvPut>(id, std::move(buffer), Request.Deadline, Request.HandleClass, Request.Tactic);
                    ev->ExtraBlockChecks = Request.ExtraBlockChecks;
                    ev->ExtraBlockChecks.emplace_back(Request.Id.TabletID(), Request.Id.Generation());
                    BDEV_QUERY(BDEV10, "TEvPut_sendToProxy", (BlobSeqId, BlobSeqId), (GroupId, groupId), (BlobId, id));
                    Agent.SendToProxy(groupId, std::move(ev), this, nullptr);
                    Agent.BytesWritten += id.BlobSize();
                    ++PutsInFlight;
                };

                if (SuppressFooter) {
                    // write the blob as is, we don't need footer for this kind
                    put(EBlobType::VG_DATA_BLOB, TRcBuf(std::move(Request.Buffer)));
                } else if (Request.Buffer.size() + sizeof(TVirtualGroupBlobFooter) <= MaxBlobSize) {
                    // write single blob with footer
                    TRope buffer = TRope(std::move(Request.Buffer));
                    buffer.Insert(buffer.End(), std::move(footerData));
                    buffer.Compact();
                    put(EBlobType::VG_COMPOSITE_BLOB, TRcBuf(std::move(buffer)));
                } else {
                    // write data blob and blob with footer
                    put(EBlobType::VG_DATA_BLOB, TRcBuf(std::move(Request.Buffer)));
                    put(EBlobType::VG_FOOTER_BLOB, TRcBuf(std::move(footerData)));
                }

                if (IssueUncertainWrites) {
                    IssueCommitBlobSeq(true);
                }

                PutsIssued = true;
            }

            void IssueCommitBlobSeq(bool uncertainWrite) {
                auto *item = CommitBlobSeq.MutableItems(0);
                if (uncertainWrite) {
                    item->SetUncertainWrite(true);
                } else {
                    item->ClearUncertainWrite();
                }

                STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA30, "IssueCommitBlobSeq", (AgentId, Agent.LogId),
                    (QueryId, GetQueryId()), (UncertainWrite, uncertainWrite), (Msg, CommitBlobSeq));

                Agent.Issue(CommitBlobSeq, this, nullptr);

                Y_ABORT_UNLESS(!WaitingForCommitBlobSeq);
                WaitingForCommitBlobSeq = true;
            }

            void RemoveBlobSeqFromInFlight() {
                STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA32, "RemoveBlobSeqFromInFlight", (AgentId, Agent.LogId),
                    (QueryId, GetQueryId()));

                Y_ABORT_UNLESS(IsInFlight);
                IsInFlight = false;

                // find and remove the write in flight record to ensure it won't be reported upon TEvPushNotify
                // reception AND to check that it wasn't already trimmed by answering TEvPushNotifyResult
                const auto it = Agent.ChannelKinds.find(NKikimrBlobDepot::TChannelKind::Data);
                if (it == Agent.ChannelKinds.end()) {
                    return EndWithError(NKikimrProto::ERROR, "no Data channels");
                }
                auto& kind = it->second;
                const size_t numErased = kind.WritesInFlight.erase(BlobSeqId);
                Y_ABORT_UNLESS(numErased || BlobSeqId.Generation < Agent.BlobDepotGeneration);
            }

            void OnUpdateBlock() override {
                CheckBlocks(); // just restart request
            }

            void OnIdAllocated(bool success) override {
                if (success) {
                    IssuePuts();
                } else {
                    EndWithError(NKikimrProto::ERROR, "out of space");
                }
            }

            void ProcessResponse(ui64 /*id*/, TRequestContext::TPtr context, TResponse response) override {
                std::visit(TOverloaded{
                    [&](TEvBlobStorage::TEvPutResult *ev) {
                        HandlePutResult(std::move(context), *ev);
                    },
                    [&](TEvBlobDepot::TEvCommitBlobSeqResult *ev) {
                        HandleCommitBlobSeqResult(std::move(context), ev->Record);
                    },
                    [&](TTabletDisconnected) {
                        EndWithError(NKikimrProto::ERROR, "BlobDepot tablet disconnected");
                    },
                    [&](TEvBlobDepot::TEvPrepareWriteS3Result *ev) {
                        HandlePrepareWriteS3Result(std::move(context), ev->Record);
                    },
                    [&](auto /*other*/) {
                        Y_ABORT("unexpected response");
                    }
                }, response);
            }

            void HandlePutResult(TRequestContext::TPtr /*context*/, TEvBlobStorage::TEvPutResult& msg) {
                STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA22, "TEvPutResult", (AgentId, Agent.LogId),
                    (QueryId, GetQueryId()), (Msg, msg));

                BDEV_QUERY(BDEV11, "TEvPut_resultFromProxy", (BlobId, msg.Id), (Status, msg.Status),
                    (ErrorReason, msg.ErrorReason));

                if (msg.Status == NKikimrProto::OK && msg.WrittenBeyondBarrier) {
                    WrittenBeyondBarrier = true;
                }

                --PutsInFlight;
                if (msg.Status != NKikimrProto::OK) {
                    EndWithError(msg.Status, std::move(msg.ErrorReason));
                } else if (PutsInFlight) {
                    // wait for all puts to complete
                } else if (BlobSeqId.Generation != Agent.BlobDepotGeneration) {
                    // FIXME: although this is error now, we can handle this in the future, when BlobDepot picks records
                    // on restarts; it may have scanned written record and already updated it in its local database;
                    // however, if it did not, we can't try to commit this records as it may be already scheduled for
                    // garbage collection by the tablet
                    EndWithError(NKikimrProto::ERROR, "BlobDepot tablet was restarting during write");
                } else if (!IssueUncertainWrites) { // proceed to second phase
                    IssueCommitBlobSeq(false);
                    RemoveBlobSeqFromInFlight();
                } else {
                    CheckIfFinished();
                }
            }

            void HandleCommitBlobSeqResult(TRequestContext::TPtr /*context*/, NKikimrBlobDepot::TEvCommitBlobSeqResult& msg) {
                STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA31, "TEvCommitBlobSeqResult", (AgentId, Agent.LogId),
                    (QueryId, GetQueryId()), (Msg, msg));

                Y_ABORT_UNLESS(WaitingForCommitBlobSeq);
                WaitingForCommitBlobSeq = false;

                Y_ABORT_UNLESS(msg.ItemsSize() == 1);
                auto& item = msg.GetItems(0);
                if (const auto status = item.GetStatus(); status != NKikimrProto::OK && status != NKikimrProto::RACE) {
                    EndWithError(item.GetStatus(), item.GetErrorReason());
                } else {
                    // it's okay to treat RACE as OK here since values are immutable in Virtual Group mode
                    CheckIfFinished();
                }
            }

            void CheckIfFinished() {
                if (!PutsInFlight && !WaitingForCommitBlobSeq) {
                    EndWithSuccess();
                }
            }

            void EndWithError(NKikimrProto::EReplyStatus status, const TString& errorReason) {
                if (BlobSeqId) {
                    BDEV_QUERY(BDEV12, "TEvPut_end", (Status, status), (ErrorReason, errorReason));
                }
                TBlobStorageQuery::EndWithError(status, errorReason);
            }

            void EndWithSuccess() {
                if (BlobSeqId) {
                    BDEV_QUERY(BDEV13, "TEvPut_end", (Status, NKikimrProto::OK));
                }

                if (IssueUncertainWrites) { // send a notification
                    auto *item = CommitBlobSeq.MutableItems(0);
                    item->SetCommitNotify(true);
                    IssueCommitBlobSeq(false);
                }

                // ensure that blob was written not beyond the barrier, or it will be lost otherwise
                Y_ABORT_UNLESS(!WrittenBeyondBarrier);

                TBlobStorageQuery::EndWithSuccess(std::make_unique<TEvBlobStorage::TEvPutResult>(NKikimrProto::OK, Request.Id,
                    Agent.GetStorageStatusFlags(), TGroupId::FromValue(Agent.VirtualGroupId), Agent.GetApproximateFreeSpaceShare()));
            }

            ui64 GetTabletId() const override {
                return Request.Id.TabletID();
            }

            ////////////////////////////////////////////////////////////////////////////////////////////////////////////
            // writing directly to S3

            void IssueS3Put() {
                NKikimrBlobDepot::TEvPrepareWriteS3 query;
                auto *item = query.AddItems();
                item->SetKey(Request.Id.AsBinaryString());
                for (const auto& [tabletId, generation] : Request.ExtraBlockChecks) {
                    auto *p = item->AddExtraBlockChecks();
                    p->SetTabletId(tabletId);
                    p->SetGeneration(generation);
                }
                item->SetLen(Request.Id.BlobSize());
                Agent.Issue(query, this, nullptr);
            }

            void HandlePrepareWriteS3Result(TRequestContext::TPtr /*context*/, NKikimrBlobDepot::TEvPrepareWriteS3Result& msg) {
#ifndef KIKIMR_DISABLE_S3_OPS
                Y_ABORT_UNLESS(msg.ItemsSize() == 1);
                const auto& item = msg.GetItems(0);
                if (item.GetStatus() != NKikimrProto::OK) {
                    return EndWithError(item.GetStatus(), item.GetErrorReason());
                }

                auto *commitItem = CommitBlobSeq.MutableItems(0);
                auto *locator = commitItem->MutableS3Locator();
                locator->CopyFrom(item.GetS3Locator());

                LocatorInFlight.emplace(TS3Locator::FromProto(*locator));

                TString key = TS3Locator::FromProto(*locator).MakeObjectName(Agent.S3BasePath);

                STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA54, "starting WriteActor", (AgentId, Agent.LogId),
                    (QueryId, GetQueryId()), (Key, key));

                WriterActorId = IssueWriteS3(std::move(key), std::move(Request.Buffer), Request.Id);

                ConnectionInstanceOnStart = Agent.ConnectionInstance;
#else
                Y_UNUSED(msg);
                Y_ABORT("S3 is not supported");
#endif
            }

            void OnPutS3ObjectResponse(std::optional<TString>&& error) override {
                STLOG(error ? PRI_WARN : PRI_DEBUG, BLOB_DEPOT_AGENT, BDA53, "OnPutS3ObjectResponse",
                    (AgentId, Agent.LogId), (QueryId, GetQueryId()), (Error, error));

                WriterActorId = {};

                if (ConnectionInstanceOnStart != Agent.ConnectionInstance) {
                    error = "BlobDepot tablet disconnected";
                    LocatorInFlight.reset(); // prevent discarding this locator
                }

                if (error) {
                    ++*Agent.S3PutsError;

                    // LocatorInFlight is not reset here on purpose: OnDestroy will generate spoiled blob message to the
                    // tablet
                    EndWithError(NKikimrProto::ERROR, TStringBuilder() << "failed to put object to S3: " << *error);
                } else {
                    ++*Agent.S3PutsOk;
                    *Agent.S3PutBytesOk += LocatorInFlight->Len;

                    LocatorInFlight.reset();
                    IssueCommitBlobSeq(false);
                }
            }
        };

        return new TPutQuery(*this, std::move(ev), received);
    }

} // NKikimr::NBlobDepot
