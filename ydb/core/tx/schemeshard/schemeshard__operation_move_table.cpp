#include "schemeshard__operation_common.h"
#include "schemeshard__operation_part.h"
#include "schemeshard_impl.h"

#include <ydb/core/mind/hive/hive.h>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

class TConfigureParts: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TMoveTable TConfigureParts"
            << ", operationId: " << OperationId;
    }

public:
    TConfigureParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {});
    }

    template<typename TEvent>
    bool HandleReplyImpl(TEvent& ev, TOperationContext& context) {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvProposeTransactionResult"
                               << " at tabletId# " << ssId);
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    DebugHint() << " HandleReply TEvProposeTransactionResult"
                                << " message# " << ev->Get()->Record.ShortDebugString());

        if (!NTableState::CollectProposeTransactionResults(OperationId, ev, context)) {
            return false;
        }

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxMoveTable);
        Y_ABORT_UNLESS(txState->MinStep); // we have to have right minstep

        return true;
    }

    bool HandleReply(TEvDataShard::TEvProposeTransactionResult::TPtr& ev, TOperationContext& context) override {
        return HandleReplyImpl(ev, context);
    }

    bool HandleReply(TEvColumnShard::TEvProposeTransactionResult::TPtr& ev, TOperationContext& context) override {
        return HandleReplyImpl(ev, context);
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", at tablet# " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxMoveTable);
        Y_ABORT_UNLESS(txState->SourcePathId);

        TPath dstPath = TPath::Init(txState->TargetPathId, context.SS);
        Y_ABORT_UNLESS(dstPath.IsResolved());
        TPath srcPath = TPath::Init(txState->SourcePathId, context.SS);
        Y_ABORT_UNLESS(srcPath.IsResolved());
        Y_ABORT_UNLESS(srcPath->IsTable() || srcPath->IsColumnTable());

        NIceDb::TNiceDb db(context.GetDB());

        // txState catches table shards
        if (!txState->Shards) {
            std::vector<TShardIdx> shardIdxs;
            if (srcPath->IsTable()) {
                const auto& srcTable = context.SS->Tables.at(srcPath->PathId);
                shardIdxs.reserve(srcTable->GetPartitions().size());
                for (const auto& shard : srcTable->GetPartitions()) {
                    shardIdxs.emplace_back(shard.ShardIdx);
                }
            } else if (srcPath->IsColumnTable()) {
                const auto& srcTable =context.SS->ColumnTables.GetVerified(srcPath.Base()->PathId);
                shardIdxs.reserve(srcTable->GetShardIdsSet().size());
                for (const auto& id: srcTable->GetShardIdsSet()) {
                    shardIdxs.emplace_back(context.SS->TabletIdToShardIdx.at(TTabletId(id)));
                }
            } else {
                Y_ABORT();
            }
            const auto tabletType = srcPath->IsTable() ? ETabletType::DataShard : ETabletType::ColumnShard;
            for (const auto& shardIdx : shardIdxs) {
                TShardInfo& shardInfo = context.SS->ShardInfos[shardIdx];

                txState->Shards.emplace_back(shardIdx, tabletType, TTxState::ConfigureParts);

                shardInfo.CurrentTxId = OperationId.GetTxId();
                context.SS->PersistShardTx(db, shardIdx, OperationId.GetTxId());
            }
            context.SS->PersistTxState(db, OperationId);
        }
        Y_ABORT_UNLESS(txState->Shards.size());

        const auto& seqNo = context.SS->StartRound(*txState);

        TString txBody;
        if (srcPath->IsTable()) {
            TTableInfo::TPtr srcTable = context.SS->Tables.at(srcPath->PathId);

            NKikimrTxDataShard::TFlatSchemeTransaction tx;
            context.SS->FillSeqNo(tx, seqNo);
            auto move = tx.MutableMoveTable();
            srcPath->PathId.ToProto(move->MutablePathId());
            move->SetTableSchemaVersion(srcTable->AlterVersion+1);

            dstPath->PathId.ToProto(move->MutableDstPathId());
            move->SetDstPath(TPath::Init(dstPath->PathId, context.SS).PathString());

            for (const auto& child: srcPath->GetChildren()) {
                auto name = child.first;

                TPath srcChildPath = srcPath.Child(name);
                Y_ABORT_UNLESS(srcChildPath.IsResolved());

                if (srcChildPath.IsDeleted()) {
                    continue;
                }
                if (srcChildPath.IsSequence()) {
                    continue;
                }

                TPath dstIndexPath = dstPath.Child(name);
                Y_ABORT_UNLESS(dstIndexPath.IsResolved());

                auto remap = move->AddReMapIndexes();
                srcChildPath->PathId.ToProto(remap->MutableSrcPathId());
                dstIndexPath->PathId.ToProto(remap->MutableDstPathId());
            }
            Y_PROTOBUF_SUPPRESS_NODISCARD tx.SerializeToString(&txBody);
        } else if (srcPath->IsColumnTable()) {
            NKikimrTxColumnShard::TSchemaTxBody tx;
            context.SS->FillSeqNo(tx, seqNo);
            auto move = tx.MutableMoveTable();
            move->SetSrcPathId(srcPath->PathId.LocalPathId);
            move->SetDstPathId(dstPath->PathId.LocalPathId);
            Y_PROTOBUF_SUPPRESS_NODISCARD tx.SerializeToString(&txBody);
        } else {
            Y_ABORT();
        }
        // send messages
        txState->ClearShardsInProgress();
        for (const auto& shard: txState->Shards) {
            auto idx = shard.Idx;
            auto tabletId = context.SS->ShardInfos[idx].TabletID;
            auto event = context.SS->MakeShardProposal(dstPath, OperationId, seqNo, txBody, context.Ctx);
            context.OnComplete.BindMsgToPipe(OperationId, tabletId, idx, event.Release());
        }
        txState->UpdateShardsInProgress(TTxState::ConfigureParts);
        return false;
    }
};

void MarkSrcDropped(NIceDb::TNiceDb& db,
                    TOperationContext& context,
                    TOperationId operationId,
                    const TTxState& txState,
                    TPath& srcPath)
{
    const auto isBackupTable = context.SS->IsBackupTable(srcPath->PathId);
    DecAliveChildrenDirect(operationId, srcPath.Parent().Base(), context, isBackupTable);
    srcPath.DomainInfo()->DecPathsInside(context.SS, 1, isBackupTable);

    srcPath->SetDropped(txState.PlanStep, operationId.GetTxId());
    context.SS->PersistDropStep(db, srcPath->PathId, txState.PlanStep, operationId);
    if (srcPath->IsTable()) {
        context.SS->Tables.at(srcPath->PathId)->DetachShardsStats();
    }
    context.SS->PersistRemoveTable(db, srcPath->PathId, context.Ctx);
    context.SS->PersistUserAttributes(db, srcPath->PathId, srcPath->UserAttrs, nullptr);

    IncParentDirAlterVersionWithRepublish(operationId, srcPath, context);
}

class TPropose: public TSubOperationState {
private:
    TOperationId OperationId;
    TTxState::ETxState& NextState;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TMoveTable TPropose"
            << ", operationId: " << OperationId;
    }
public:
    TPropose(TOperationId id, TTxState::ETxState& nextState)
        : OperationId(id)
        , NextState(nextState)
    {
        IgnoreMessages(DebugHint(), {TEvHive::TEvCreateTabletReply::EventType, TEvDataShard::TEvProposeTransactionResult::EventType, TEvColumnShard::TEvProposeTransactionResult::EventType});
    }

    template<typename TEvent>
    bool HandleReplyImpl(TEvent& ev, TOperationContext& context) {
        TTabletId ssId = context.SS->SelfTabletId();
        const auto& evRecord = ev->Get()->Record;

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     DebugHint() << " HandleReply " << TEvSchemaChangedTraits<TEvent>::GetName()
                     << " at tablet: " << ssId);
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    DebugHint() << " HandleReply " << TEvSchemaChangedTraits<TEvent>::GetName()
                     << " triggered early"
                     << ", message: " << evRecord.ShortDebugString());

        NTableState::CollectSchemaChanged(OperationId, ev, context);
        return false;
    }

    bool HandleReply(TEvDataShard::TEvSchemaChanged::TPtr& ev, TOperationContext& context) override {
        return HandleReplyImpl(ev, context);
    }

    bool HandleReply(TEvColumnShard::TEvNotifyTxCompletionResult::TPtr& ev, TOperationContext& context) override {
        return HandleReplyImpl(ev, context);
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        TStepId step = TStepId(ev->Get()->StepId);
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvOperationPlan"
                               << ", step: " << step
                               << ", at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxMoveTable);

        auto srcPath = TPath::Init(txState->SourcePathId, context.SS);
        auto dstPath = TPath::Init(txState->TargetPathId, context.SS);

        NIceDb::TNiceDb db(context.GetDB());

        txState->PlanStep = step;
        context.SS->PersistTxPlanStep(db, OperationId, step);

        // move shards
        for (const auto& shard : txState->Shards) {
            auto shardIdx = shard.Idx;
            TShardInfo& shardInfo = context.SS->ShardInfos[shardIdx];

            shardInfo.PathId = dstPath->PathId;
            context.SS->DecrementPathDbRefCount(srcPath.Base()->PathId, "move shard");
            context.SS->IncrementPathDbRefCount(dstPath.Base()->PathId, "move shard");
            context.SS->PersistShardPathId(db, shardIdx, dstPath.Base()->PathId);

            srcPath.Base()->DecShardsInside();
            dstPath.Base()->IncShardsInside();
        }

        Y_ABORT_UNLESS(!context.SS->Tables.contains(dstPath.Base()->PathId));
        if (srcPath->IsTable()) {
            Y_ABORT_UNLESS(context.SS->Tables.contains(srcPath.Base()->PathId));

            TTableInfo::TPtr tableInfo = TTableInfo::DeepCopy(*context.SS->Tables.at(srcPath.Base()->PathId));
            tableInfo->ResetDescriptionCache();
            tableInfo->AlterVersion += 1;

            // copy table info
            context.SS->Tables[dstPath.Base()->PathId] = tableInfo;
            context.SS->PersistTable(db, dstPath.Base()->PathId);
            context.SS->PersistTablePartitionStats(db, dstPath.Base()->PathId, tableInfo);
        } else if (srcPath->IsColumnTable()) {
            auto srcTable = context.SS->ColumnTables.GetVerified(srcPath.Base()->PathId);
            auto tableInfo = context.SS->ColumnTables.BuildNew(dstPath.Base()->PathId, srcTable.GetPtr());
            tableInfo->AlterVersion += 1;
            context.SS->PersistColumnTable(db, dstPath.Base()->PathId, *tableInfo, false);
        } else {
            Y_ABORT();
        }
        context.SS->IncrementPathDbRefCount(dstPath.Base()->PathId, "move table info");

        dstPath->StepCreated = step;
        context.SS->PersistCreateStep(db, dstPath.Base()->PathId, step);
        dstPath.DomainInfo()->IncPathsInside(context.SS);

        dstPath.Activate();
        IncParentDirAlterVersionWithRepublish(OperationId, dstPath, context);

        NextState = TTxState::WaitShadowPathPublication;
        context.SS->ChangeTxState(db, OperationId, TTxState::WaitShadowPathPublication);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxMoveTable);
        Y_ABORT_UNLESS(txState->SourcePathId);
        Y_ABORT_UNLESS(txState->MinStep);

        TSet<TTabletId> shardSet;
        for (const auto& shard : txState->Shards) {
            TShardIdx idx = shard.Idx;
            TTabletId tablet = context.SS->ShardInfos.at(idx).TabletID;
            TPath srcPath = TPath::Init(txState->SourcePathId, context.SS);
            if (srcPath->IsColumnTable()) {
                auto event = std::make_unique<TEvColumnShard::TEvNotifyTxCompletion>(ui64(OperationId.GetTxId()));
                context.OnComplete.BindMsgToPipe(OperationId, tablet, shard.Idx, event.release());
            }
            shardSet.insert(tablet);
        }

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, txState->MinStep, std::move(shardSet));
        return false;
    }
};

class TWaitRenamedPathPublication: public TSubOperationState {
private:
    TOperationId OperationId;

    TPathId ActivePathId;

    TString DebugHint() const override {
        return TStringBuilder()
                << "TMoveTable TWaitRenamedPathPublication"
                << " operationId: " << OperationId;
    }

public:
    TWaitRenamedPathPublication(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {TEvHive::TEvCreateTabletReply::EventType, TEvDataShard::TEvProposeTransactionResult::EventType, TEvPrivate::TEvOperationPlan::EventType});
    }

    template<typename TEvent>
    bool HandleReplyImpl(TEvent& ev, TOperationContext& context) {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply " << TEvSchemaChangedTraits<TEvent>::GetName()
                               << ", save it"
                               << ", at schemeshard: " << ssId);

        NTableState::CollectSchemaChanged(OperationId, ev, context);
        return false;
    }
    bool HandleReply(TEvDataShard::TEvSchemaChanged::TPtr& ev, TOperationContext& context) override {
        return HandleReplyImpl(ev, context);
    }

    bool HandleReply(TEvColumnShard::TEvNotifyTxCompletionResult::TPtr& ev, TOperationContext& context) override {
        return HandleReplyImpl(ev, context);
    }

    bool HandleReply(TEvPrivate::TEvCompletePublication::TPtr& ev, TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvPrivate::TEvCompletePublication"
                               << ", msg: " << ev->Get()->ToString()
                               << ", at tablet# " << ssId);

        Y_ABORT_UNLESS(ActivePathId == ev->Get()->PathId);

        NIceDb::TNiceDb db(context.GetDB());
        context.SS->ChangeTxState(db, OperationId, TTxState::DeletePathBarrier);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();
        context.OnComplete.RouteByTabletsFromOperation(OperationId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", operation type: " << TTxState::TypeName(txState->TxType)
                               << ", at tablet# " << ssId);

        TPath srcPath = TPath::Init(txState->SourcePathId, context.SS);

        if (srcPath.IsActive()) {
            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                        DebugHint() << " ProgressState"
                                    << ", no renaming has been detected for this operation");

            NIceDb::TNiceDb db(context.GetDB());
            context.SS->ChangeTxState(db, OperationId, TTxState::DeletePathBarrier);
            return true;
        }

        auto activePath = TPath::Resolve(srcPath.PathString(), context.SS);
        Y_ABORT_UNLESS(activePath.IsResolved());

        Y_ABORT_UNLESS(activePath != srcPath);

        ActivePathId = activePath->PathId;
        context.OnComplete.PublishAndWaitPublication(OperationId, activePath->PathId);

        return false;
    }
};

class TDeleteTableBarrier: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
                << "TMoveTable TDeleteTableBarrier"
                << " operationId: " << OperationId;
    }

public:
    TDeleteTableBarrier(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {TEvHive::TEvCreateTabletReply::EventType, TEvDataShard::TEvProposeTransactionResult::EventType, TEvPrivate::TEvOperationPlan::EventType});
    }

    template<typename TEvent>
    bool HandleReplyImpl(TEvent& ev, TOperationContext& context) {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply " << TEvSchemaChangedTraits<TEvent>::GetName()
                               << ", save it"
                               << ", at schemeshard: " << ssId);

        NTableState::CollectSchemaChanged(OperationId, ev, context);
        return false;
    }

    bool HandleReply(TEvDataShard::TEvSchemaChanged::TPtr& ev, TOperationContext& context) override {
        return HandleReplyImpl(ev, context);
    }

    bool HandleReply(TEvColumnShard::TEvNotifyTxCompletionResult::TPtr& ev, TOperationContext& context) override {
        return HandleReplyImpl(ev, context);
    }

    bool HandleReply(TEvPrivate::TEvCompleteBarrier::TPtr& ev, TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvPrivate:TEvCompleteBarrier"
                               << ", msg: " << ev->Get()->ToString()
                               << ", at tablet# " << ssId);

        NIceDb::TNiceDb db(context.GetDB());

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);

        auto srcPath = TPath::Init(txState->SourcePathId, context.SS);
        auto dstPath = TPath::Init(txState->TargetPathId, context.SS);

        Y_ABORT_UNLESS(txState->PlanStep);

        MarkSrcDropped(db, context, OperationId, *txState, srcPath);
        if (srcPath->IsTable()) {
            Y_ABORT_UNLESS(context.SS->Tables.contains(dstPath.Base()->PathId));
            auto tableInfo = context.SS->Tables.at(dstPath.Base()->PathId);

            if (tableInfo->IsTTLEnabled() && !context.SS->TTLEnabledTables.contains(dstPath.Base()->PathId)) {
                context.SS->TTLEnabledTables[dstPath.Base()->PathId] = tableInfo;
                // MarkSrcDropped() removes srcPath from TTLEnabledTables & decrements the counters
                context.SS->TabletCounters->Simple()[COUNTER_TTL_ENABLED_TABLE_COUNT].Add(1);

                const auto now = context.Ctx.Now();
                for (auto& shard : tableInfo->GetPartitions()) {
                    auto& lag = shard.LastCondEraseLag;
                    lag = now - shard.LastCondErase;
                    context.SS->TabletCounters->Percentile()[COUNTER_NUM_SHARDS_BY_TTL_LAG].IncrementFor(lag->Seconds());
                }
            }
        }

        context.SS->ChangeTxState(db, OperationId, TTxState::ProposedWaitParts);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();
        context.OnComplete.RouteByTabletsFromOperation(OperationId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", operation type: " << TTxState::TypeName(txState->TxType)
                               << ", at tablet# " << ssId);

        context.OnComplete.Barrier(OperationId, "RenamePathBarrier");
        return false;
    }
};

class TDone: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TMoveTable TDone"
            << ", operationId: " << OperationId;
    }
public:
    TDone(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), AllIncomingEvents());
    }
    template<typename TEvent>
    bool HandleReplyImpl(TEvent& ev, TOperationContext& context) {
        TTabletId ssId = context.SS->SelfTabletId();
        const TActorId& ackTo = ev->Sender;

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply " << TEvSchemaChangedTraits<TEvent>::GetName()
                               << " repeated message, ack it anyway"
                               << " at tablet: " << ssId);

        THolder<TEvDataShard::TEvSchemaChangedResult> event = MakeHolder<TEvDataShard::TEvSchemaChangedResult>();
        event->Record.SetTxId(ui64(OperationId.GetTxId()));

        context.OnComplete.Send(ackTo, std::move(event));
        return false;
    }
    
    bool HandleReply(TEvDataShard::TEvSchemaChanged::TPtr& ev, TOperationContext& context) override {
        return HandleReplyImpl(ev, context);
    }

    bool HandleReply(TEvColumnShard::TEvNotifyTxCompletionResult::TPtr& ev, TOperationContext& context) override {
        return HandleReplyImpl(ev, context);
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxMoveTable);

        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", SourcePathId: " << txState->SourcePathId
                               << ", TargetPathId: " << txState->TargetPathId
                               << ", at schemeshard: " << ssId);

        // clear resources on src
        NIceDb::TNiceDb db(context.GetDB());
        TPathElement::TPtr srcPath = context.SS->PathsById.at(txState->SourcePathId);
        context.OnComplete.ReleasePathState(OperationId, srcPath->PathId, TPathElement::EPathState::EPathStateNotExist);

        TPathElement::TPtr dstPath = context.SS->PathsById.at(txState->TargetPathId);
        context.OnComplete.ReleasePathState(OperationId, dstPath->PathId, TPathElement::EPathState::EPathStateNoChanges);

        context.OnComplete.DoneOperation(OperationId);
        return true;
    }
};

class TMoveTable: public TSubOperation {
    TTxState::ETxState AfterPropose = TTxState::Invalid;

    static TTxState::ETxState NextState() {
        return TTxState::ConfigureParts;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) const override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::ConfigureParts:
            return TTxState::Propose;
        case TTxState::Propose:
            return AfterPropose;
        case TTxState::WaitShadowPathPublication:
            return TTxState::DeletePathBarrier;
        case TTxState::DeletePathBarrier:
            return TTxState::ProposedWaitParts;
        case TTxState::ProposedWaitParts:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::ConfigureParts:
            return MakeHolder<TConfigureParts>(OperationId);
        case TTxState::Propose:
            return MakeHolder<TPropose>(OperationId, AfterPropose);
        case TTxState::WaitShadowPathPublication:
            return MakeHolder<TWaitRenamedPathPublication>(OperationId);
        case TTxState::DeletePathBarrier:
            return MakeHolder<TDeleteTableBarrier>(OperationId);
        case TTxState::ProposedWaitParts:
            return MakeHolder<NTableState::TProposedWaitParts>(OperationId);
        case TTxState::Done:
            return MakeHolder<TDone>(OperationId);
        default:
            return nullptr;
        }
    }

public:
    using TSubOperation::TSubOperation;

    THolder<TProposeResponse> Propose(const TString&, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        const auto acceptExisted = !Transaction.GetFailOnExist();
        const auto& opDescr = Transaction.GetMoveTable();

        const TString& srcPathStr = opDescr.GetSrcPath();
        const TString& dstPathStr = opDescr.GetDstPath();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TMoveTable Propose"
                         << ", from: "<< srcPathStr
                         << ", to: " << dstPathStr
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << ssId);

        THolder<TProposeResponse> result;
        result.Reset(new TEvSchemeShard::TEvModifySchemeTransactionResult(
            NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(ssId)));

        TString errStr;

        TPath srcPath = TPath::Resolve(srcPathStr, context.SS);
        {
            if (!srcPath->IsTable() && !srcPath->IsColumnTable()) {
                result->SetError(NKikimrScheme::StatusPreconditionFailed, "Cannot move non-tables");
            }
            TPath::TChecker checks = srcPath.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotBackupTable()
                .NotAsyncReplicaTable()
                .NotUnderTheSameOperation(OperationId.GetTxId())
                .NotUnderOperation();

            if (!checks) {
                result->SetError(checks.GetStatus(), checks.GetError());
                return result;
            }
        }

        TPath dstPath = TPath::Resolve(dstPathStr, context.SS);
        TPath dstParent = dstPath.Parent();

        {
            TPath::TChecker checks = dstParent.Check();
            checks
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .FailOnRestrictedCreateInTempZone(Transaction.GetAllowCreateInTempDir());

                if (dstParent.IsUnderDeleting()) {
                    checks
                        .IsUnderDeleting()
                        .IsUnderTheSameOperation(OperationId.GetTxId());
                } else if (dstParent.IsUnderMoving()) {
                    // it means that dstPath is free enough to be the move destination
                    checks
                        .IsUnderMoving()
                        .IsUnderTheSameOperation(OperationId.GetTxId());
                } else if (dstParent.IsUnderCreating()) {
                    checks
                        .IsUnderCreating()
                        .IsUnderTheSameOperation(OperationId.GetTxId());
                } else {
                    checks
                        .NotUnderOperation();
                }

            if (!checks) {
                result->SetError(checks.GetStatus(), checks.GetError());
                return result;
            }
        }

        if (dstParent.IsUnderOperation()) {
            dstPath = TPath::ResolveWithInactive(OperationId, dstPathStr, context.SS);
            dstParent = dstPath.Parent();
        }

        {
            TPath::TChecker checks = dstPath.Check();
            checks.IsAtLocalSchemeShard();
            if (dstPath.IsResolved()) {
                checks
                    .IsResolved();

                if (dstPath.IsUnderDeleting()) {
                    checks
                        .IsUnderDeleting()
                        .IsUnderTheSameOperation(OperationId.GetTxId());
                } else if (dstPath.IsUnderMoving()) {
                    // it means that dstPath is free enough to be the move destination
                    checks
                        .IsUnderMoving()
                        .IsUnderTheSameOperation(OperationId.GetTxId());
                } else {
                    checks
                        .NotUnderTheSameOperation(OperationId.GetTxId())
                        .FailOnExist(srcPath->IsColumnTable() ? TPathElement::EPathType::EPathTypeColumnTable : TPathElement::EPathType::EPathTypeTable, acceptExisted);
                }
            } else {
                checks
                    .NotEmpty()
                    .NotResolved();
            }

            if (checks) {
                checks
                    .DepthLimit()
                    .IsValidLeafName(context.UserToken.Get());
            }

            if (!checks) {
                result->SetError(checks.GetStatus(), checks.GetError());
                return result;
            }
        }

        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }

        if (!context.SS->CheckLocks(srcPath.Base()->PathId, Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusMultipleModifications, errStr);
            return result;
        }

        auto guard = context.DbGuard();
        TPathId allocatedPathId = context.SS->AllocatePathId();
        context.MemChanges.GrabNewPath(context.SS, allocatedPathId);
        context.MemChanges.GrabPath(context.SS, dstParent.Base()->PathId);
        context.MemChanges.GrabPath(context.SS, srcPath.Base()->PathId);
        context.MemChanges.GrabPath(context.SS, srcPath.Base()->ParentPathId);
        context.MemChanges.GrabNewTxState(context.SS, OperationId);
        context.MemChanges.GrabNewIndex(context.SS, allocatedPathId);

        context.DbChanges.PersistPath(allocatedPathId);
        context.DbChanges.PersistPath(dstParent.Base()->PathId);
        context.DbChanges.PersistPath(srcPath.Base()->PathId);
        context.DbChanges.PersistPath(srcPath.Base()->ParentPathId);
        context.DbChanges.PersistApplyUserAttrs(allocatedPathId);
        context.DbChanges.PersistTxState(OperationId);

        // create new path and inherit properties from src
        dstPath.MaterializeLeaf(srcPath.Base()->Owner, allocatedPathId, /*allowInactivePath*/ true);
        result->SetPathId(dstPath.Base()->PathId.LocalPathId);
        dstPath.Base()->CreateTxId = OperationId.GetTxId();
        dstPath.Base()->LastTxId = OperationId.GetTxId();
        dstPath.Base()->PathState = TPathElement::EPathState::EPathStateCreate;
        dstPath.Base()->PathType = srcPath.Base()->PathType;
        dstPath.Base()->UserAttrs->AlterData = srcPath.Base()->UserAttrs;
        dstPath.Base()->ACL = srcPath.Base()->ACL;

        IncAliveChildrenSafeWithUndo(OperationId, dstParent, context); // for correct discard of ChildrenExist prop

        // create tx state, do not catch shards right now
        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxMoveTable, dstPath.Base()->PathId, srcPath.Base()->PathId);
        txState.State = TTxState::ConfigureParts;

        srcPath->PathState = TPathElement::EPathState::EPathStateMoving;
        srcPath->LastTxId = OperationId.GetTxId();

        IncParentDirAlterVersionWithRepublishSafeWithUndo(OperationId, dstPath, context.SS, context.OnComplete);
        IncParentDirAlterVersionWithRepublishSafeWithUndo(OperationId, srcPath, context.SS, context.OnComplete);

        // wait splits
        if (srcPath->IsTable()) {
            TTableInfo::TPtr tableSrc = context.SS->Tables.at(srcPath.Base()->PathId);
            for (auto splitTx: tableSrc->GetSplitOpsInFlight()) {
                context.OnComplete.Dependence(splitTx.GetTxId(), OperationId.GetTxId());
            }
        }
        context.OnComplete.ActivateTx(OperationId);

        SetState(NextState());
        return result;
    }

    void AbortPropose(TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TMoveTable AbortPropose"
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << context.SS->TabletID());
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TMoveTable AbortUnsafe"
                         << ", opId: " << OperationId
                         << ", forceDropId: " << forceDropTxId
                         << ", at schemeshard: " << context.SS->TabletID());

        context.OnComplete.DoneOperation(OperationId);
    }
};

}

namespace NKikimr::NSchemeShard {

ISubOperation::TPtr CreateMoveTable(TOperationId id, const TTxTransaction& tx) {
    return MakeSubOperation<TMoveTable>(id, tx);
}

ISubOperation::TPtr CreateMoveTable(TOperationId id, TTxState::ETxState state) {
    Y_ABORT_UNLESS(state != TTxState::Invalid);
    return MakeSubOperation<TMoveTable>(id, state);
}

}
