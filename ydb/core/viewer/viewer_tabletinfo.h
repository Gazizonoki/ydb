#pragma once
#include "json_wb_req.h"
#include <ydb/core/scheme/scheme_types_proto.h>
#include <ydb/core/util/wildcard.h>

namespace NKikimr::NViewer {

template<>
struct TWhiteboardInfo<NKikimrWhiteboard::TEvTabletStateResponse> {
    using TResponseEventType = TEvWhiteboard::TEvTabletStateResponse;
    using TResponseType = NKikimrWhiteboard::TEvTabletStateResponse;
    using TElementType = NKikimrWhiteboard::TTabletStateInfo;
    using TElementTypePacked5 = NNodeWhiteboard::TEvWhiteboard::TEvTabletStateResponsePacked5;
    using TElementKeyType = std::pair<ui64, ui32>;

    static constexpr bool StaticNodesOnly = false;

    static ::google::protobuf::RepeatedPtrField<TElementType>& GetElementsField(TResponseType& response) {
        return *response.MutableTabletStateInfo();
    }

    static std::span<const TElementTypePacked5> GetElementsFieldPacked5(const TResponseType& response) {
        const auto& packed5 = response.GetPacked5();
        return std::span{reinterpret_cast<const TElementTypePacked5*>(packed5.data()), packed5.size() / sizeof(TElementTypePacked5)};
    }

    static size_t GetElementsCount(const TResponseType& response) {
        return response.GetTabletStateInfo().size() + response.GetPacked5().size() / sizeof(TElementTypePacked5);
    }

    static TElementKeyType GetElementKey(const TElementType& type) {
        return TElementKeyType(type.GetTabletId(), type.GetFollowerId());
    }

    static TElementKeyType GetElementKey(const TElementTypePacked5& type) {
        return TElementKeyType(type.TabletId, type.FollowerId);
    }

    static TString GetDefaultMergeField() {
        return "TabletId,FollowerId";
    }

    static void MergeResponses(TResponseType& result, TMap<ui32, TResponseType>& responses, const TString& fields = GetDefaultMergeField()) {
        if (fields == GetDefaultMergeField()) {
            TStaticMergeKey<TResponseType> mergeKey;
            TWhiteboardMerger<TResponseType>::MergeResponsesBaseHybrid(result, responses, mergeKey);
        } else {
            TWhiteboardMerger<TResponseType>::TDynamicMergeKey mergeKey(fields);
            TWhiteboardMerger<TResponseType>::MergeResponsesBase(result, responses, mergeKey);
        }
    }
};

template <>
struct TWhiteboardMergerComparator<NKikimrWhiteboard::TTabletStateInfo> {
    bool operator ()(const NKikimrWhiteboard::TTabletStateInfo& a, const NKikimrWhiteboard::TTabletStateInfo& b) const {
        return std::make_tuple(a.GetGeneration(), a.GetChangeTime()) < std::make_tuple(b.GetGeneration(), b.GetChangeTime());
    }
};

template <>
struct TWhiteboardMergerComparator<NNodeWhiteboard::TEvWhiteboard::TEvTabletStateResponsePacked5> {
    bool operator ()(const NNodeWhiteboard::TEvWhiteboard::TEvTabletStateResponsePacked5& a, const NNodeWhiteboard::TEvWhiteboard::TEvTabletStateResponsePacked5& b) const {
        return a.Generation < b.Generation;
    }
};

class TJsonTabletInfo : public TJsonWhiteboardRequest<TEvWhiteboard::TEvTabletStateRequest, TEvWhiteboard::TEvTabletStateResponse> {
    static const bool WithRetry = false;
    bool ReplyWithDeadTabletsInfo;
    using TBase = TJsonWhiteboardRequest<TEvWhiteboard::TEvTabletStateRequest, TEvWhiteboard::TEvTabletStateResponse>;
    using TThis = TJsonTabletInfo;
    THashMap<ui64, NKikimrTabletBase::TTabletTypes::EType> Tablets;
    std::unordered_set<ui64> DeadTablets;
    std::unordered_map<ui64, TString> EndOfRangeKeyPrefix;
    TTabletId HiveId = 0;
    bool IsBase64Encode = true;
    NKikimr::TSubDomainKey FilterTenantId;

public:
    TJsonTabletInfo(IViewer *viewer, NMon::TEvHttpInfo::TPtr &ev)
        : TJsonWhiteboardRequest(viewer, ev)
    {
        static TString prefix = "json/tabletinfo ";
        LogPrefix = prefix;
    }

    void Bootstrap() override {
        if (NeedToRedirect()) {
            return;
        }
        const auto& params(Event->Get()->Request.GetParams());
        TBase::RequestSettings.Timeout = FromStringWithDefault<ui32>(params.Get("timeout"), 10000);
        if (DatabaseNavigateResponse && DatabaseNavigateResponse->IsOk()) {
            TPathId domainRoot;
            if (AppData()) {
                TIntrusivePtr<TDomainsInfo> domains = AppData()->DomainsInfo;
                auto* domain = domains->GetDomain();
                if (domain) {
                    domainRoot = TPathId(domain->SchemeRoot, 1);
                }
            }
            if (DatabaseNavigateResponse->Get()->Request->ResultSet.front().DomainInfo->DomainKey != domainRoot) {
                const TPathId& pathId(DatabaseNavigateResponse->Get()->Request->ResultSet.front().DomainInfo->DomainKey);
                FilterTenantId.first = pathId.OwnerId;
                FilterTenantId.second = pathId.LocalPathId;
            }
        }
        if (DatabaseBoardInfoResponse && DatabaseBoardInfoResponse->IsOk()) {
            TBase::RequestSettings.FilterNodeIds = TBase::GetNodesFromBoardReply(DatabaseBoardInfoResponse->GetRef());
        } else if (ResourceBoardInfoResponse && ResourceBoardInfoResponse->IsOk()) {
            TBase::RequestSettings.FilterNodeIds = TBase::GetNodesFromBoardReply(ResourceBoardInfoResponse->GetRef());
        } else if (Database || SharedDatabase) {
            RequestStateStorageEndpointsLookup(SharedDatabase ? SharedDatabase : Database);
            Become(&TThis::StateRequestedLookup, TDuration::MilliSeconds(TBase::RequestSettings.Timeout), new TEvents::TEvWakeup());
            return;
        }
        CheckPath();
    }

    void CheckPath() {
        BLOG_TRACE("CheckPath()");
        const auto& params(Event->Get()->Request.GetParams());
        ReplyWithDeadTabletsInfo = params.Has("path");
        if (params.Has("path")) {
            TBase::RequestSettings.Timeout = FromStringWithDefault<ui32>(params.Get("timeout"), 10000);
            IsBase64Encode = FromStringWithDefault<bool>(params.Get("base64"), IsBase64Encode);
            NKikimrSchemeOp::TDescribeOptions options;
            options.SetReturnBoundaries(true);
            options.SetReturnIndexTableBoundaries(true);
            options.SetShowPrivateTable(true);
            RequestTxProxyDescribe(params.Get("path"), options);
            Become(&TThis::StateRequestedDescribe, TDuration::MilliSeconds(TBase::RequestSettings.Timeout), new TEvents::TEvWakeup());
        } else {
            TBase::Bootstrap();
        }
    }

    THolder<TEvWhiteboard::TEvTabletStateRequest> BuildRequest() override {
        THolder<TEvWhiteboard::TEvTabletStateRequest> request = TBase::BuildRequest();
        if (!TBase::RequestSettings.FilterFields.empty()) {
            if (IsMatchesWildcard(TBase::RequestSettings.FilterFields, "(TabletId=*)")) {
                TString strTabletId(TBase::RequestSettings.FilterFields.substr(10, TBase::RequestSettings.FilterFields.size() - 11));
                TTabletId uiTabletId(FromStringWithDefault<TTabletId>(strTabletId, {}));
                if (uiTabletId) {
                    Tablets[uiTabletId] = NKikimrTabletBase::TTabletTypes::Unknown;
                    request->Record.AddFilterTabletId(uiTabletId);
                }
            }
        }
        if (FilterTenantId) {
            request->Record.MutableFilterTenantId()->SetSchemeShard(FilterTenantId.GetSchemeShard());
            request->Record.MutableFilterTenantId()->SetPathId(FilterTenantId.GetPathId());
        }
        return request;
    }

    void Handle(TEvStateStorage::TEvBoardInfo::TPtr& ev) {
        TBase::RequestSettings.FilterNodeIds = TBase::GetNodesFromBoardReply(ev);
        CheckPath();
        RequestDone();
    }

    TString GetColumnValue(const TCell& cell, const NKikimrSchemeOp::TColumnDescription& type) {
        if (cell.IsNull()) {
            return "NULL";
        }
        switch (type.GetTypeId()) {
        case NScheme::NTypeIds::Int32:
            return ToString(cell.AsValue<i32>());
        case NScheme::NTypeIds::Uint32:
            return ToString(cell.AsValue<ui32>());
        case NScheme::NTypeIds::Int64:
            return ToString(cell.AsValue<i64>());
        case NScheme::NTypeIds::Uint64:
            return ToString(cell.AsValue<ui64>());
        case NScheme::NTypeIds::Int8:
            return ToString(cell.AsValue<i8>());
        case NScheme::NTypeIds::Uint8:
            return ToString(cell.AsValue<ui8>());
        case NScheme::NTypeIds::Int16:
            return ToString(cell.AsValue<i16>());
        case NScheme::NTypeIds::Uint16:
            return ToString(cell.AsValue<ui16>());
        case NScheme::NTypeIds::Bool:
            return cell.AsValue<bool>() ? "true" : "false";
        case NScheme::NTypeIds::Date:            return "Date";
        case NScheme::NTypeIds::Datetime:        return "Datetime";
        case NScheme::NTypeIds::Timestamp:       return "Timestamp";
        case NScheme::NTypeIds::Interval:        return "Interval";
        case NScheme::NTypeIds::Date32:          return "Date32";
        case NScheme::NTypeIds::Datetime64:      return "Datetime64";
        case NScheme::NTypeIds::Timestamp64:     return "Timestamp64";
        case NScheme::NTypeIds::Interval64:      return "Interval64";
        case NScheme::NTypeIds::PairUi64Ui64:    return "PairUi64Ui64";
        case NScheme::NTypeIds::String:
        case NScheme::NTypeIds::String4k:
        case NScheme::NTypeIds::String2m:
            return IsBase64Encode ? Base64Encode(cell.AsBuf()) : (TStringBuilder() << '"' << cell.AsBuf() << '"');
        case NScheme::NTypeIds::Utf8:
            return TStringBuilder() << '"' << cell.AsBuf() << '"';
        case NScheme::NTypeIds::Decimal: {
            NScheme::TTypeInfo typeInfo = NKikimr::NScheme::TypeInfoFromProto(type.GetTypeId(), type.GetTypeInfo());
            return typeInfo.GetDecimalType().CellValueToString(cell.AsValue<std::pair<ui64, i64>>());
        }
        case NScheme::NTypeIds::Pg: {
            NScheme::TTypeInfo typeInfo = NKikimr::NScheme::TypeInfoFromProto(type.GetTypeId(), type.GetTypeInfo());
            auto convert = NPg::PgNativeTextFromNativeBinary(cell.AsBuf(),typeInfo.GetPgTypeDesc());
            return TStringBuilder() << '"' << (!convert.Error ? convert.Str : *convert.Error) << '"';;
        }
        case NScheme::NTypeIds::DyNumber:        return "DyNumber";
        case NScheme::NTypeIds::Uuid:            return "Uuid";
        default:
            return "-";
        }
    }

    void Handle(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr &ev) {
        THolder<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult> describeResult = ev->Release();
        if (describeResult->GetRecord().GetStatus() == NKikimrScheme::EStatus::StatusSuccess) {
            const auto& pathDescription = describeResult->GetRecord().GetPathDescription();
            for (auto shard : pathDescription.GetColumnTableDescription().GetSharding().GetColumnShards()) {
                Tablets[shard] = NKikimrTabletBase::TTabletTypes::ColumnShard;
            }
            for (auto shard : pathDescription.GetColumnStoreDescription().GetColumnShards()) {
                Tablets[shard] = NKikimrTabletBase::TTabletTypes::ColumnShard;
            }
            if (pathDescription.HasTable()) {
                std::vector<NKikimrSchemeOp::TColumnDescription> keyColumns;
                for (uint32 id : pathDescription.GetTable().GetKeyColumnIds()) {
                    for (const auto& column : pathDescription.GetTable().GetColumns()) {
                        if (column.GetId() == id) {
                            keyColumns.push_back(column);
                            break;
                        }
                    }
                }
                for (const auto& partition : pathDescription.GetTablePartitions()) {
                    Tablets[partition.GetDatashardId()] = NKikimrTabletBase::TTabletTypes::DataShard;
                    if (partition.HasEndOfRangeKeyPrefix()) {
                        TSerializedCellVec cellVec;
                        if (TSerializedCellVec::TryParse(partition.GetEndOfRangeKeyPrefix(), cellVec)) {
                            TStringBuilder key;
                            TConstArrayRef<TCell> cells(cellVec.GetCells());
                            if (cells.size() == keyColumns.size()) {
                                if (cells.size() > 1) {
                                    key << "(";
                                }
                                for (size_t idx = 0; idx < cells.size(); ++idx) {
                                    if (idx > 0) {
                                        key << ",";
                                    }
                                    const NKikimrSchemeOp::TColumnDescription& type(keyColumns[idx]);
                                    const TCell& cell(cells[idx]);
                                    key << GetColumnValue(cell, type);
                                }
                                if (cells.size() > 1) {
                                    key << ")";
                                }
                            }
                            if (key) {
                                EndOfRangeKeyPrefix[partition.GetDatashardId()] = key;
                            }
                        }
                    }
                }
            }
            for (const auto& partition : pathDescription.GetPersQueueGroup().GetPartitions()) {
                Tablets[partition.GetTabletId()] = NKikimrTabletBase::TTabletTypes::PersQueue;
            }
            if (pathDescription.HasPersQueueGroup()) {
                Tablets[pathDescription.GetPersQueueGroup().GetBalancerTabletID()] = NKikimrTabletBase::TTabletTypes::PersQueueReadBalancer;
            }
            if (pathDescription.HasRtmrVolumeDescription()) {
                for (const auto& partition : pathDescription.GetRtmrVolumeDescription().GetPartitions()) {
                    Tablets[partition.GetTabletId()] = NKikimrTabletBase::TTabletTypes::RTMRPartition;
                }
            }
            if (pathDescription.HasBlockStoreVolumeDescription()) {
                for (const auto& partition : pathDescription.GetBlockStoreVolumeDescription().GetPartitions()) {
                    Tablets[partition.GetTabletId()] = NKikimrTabletBase::TTabletTypes::BlockStorePartition;
                }
                if (pathDescription.GetBlockStoreVolumeDescription().HasVolumeTabletId()) {
                    Tablets[pathDescription.GetBlockStoreVolumeDescription().GetVolumeTabletId()] = NKikimrTabletBase::TTabletTypes::BlockStoreVolume;
                }
            }
            if (pathDescription.GetKesus().HasKesusTabletId()) {
                Tablets[pathDescription.GetKesus().GetKesusTabletId()] = NKikimrTabletBase::TTabletTypes::Kesus;
            }
            if (pathDescription.HasSolomonDescription()) {
                for (const auto& partition : pathDescription.GetSolomonDescription().GetPartitions()) {
                    Tablets[partition.GetTabletId()] = NKikimrTabletBase::TTabletTypes::KeyValue;
                }
            }
            if (pathDescription.GetFileStoreDescription().HasIndexTabletId()) {
                Tablets[pathDescription.GetFileStoreDescription().GetIndexTabletId()] = NKikimrTabletBase::TTabletTypes::FileStore;
            }
            if (pathDescription.GetSequenceDescription().HasSequenceShard()) {
                Tablets[pathDescription.GetSequenceDescription().GetSequenceShard()] = NKikimrTabletBase::TTabletTypes::SequenceShard;
            }
            if (pathDescription.GetReplicationDescription().HasControllerId()) {
                Tablets[pathDescription.GetReplicationDescription().GetControllerId()] = NKikimrTabletBase::TTabletTypes::ReplicationController;
            }
            if (pathDescription.GetBlobDepotDescription().HasTabletId()) {
                Tablets[pathDescription.GetBlobDepotDescription().GetTabletId()] = NKikimrTabletBase::TTabletTypes::BlobDepot;
            }

            if (pathDescription.GetSelf().GetPathType() == NKikimrSchemeOp::EPathType::EPathTypeDir
                || pathDescription.GetSelf().GetPathType() == NKikimrSchemeOp::EPathType::EPathTypeSubDomain
                || pathDescription.GetSelf().GetPathType() == NKikimrSchemeOp::EPathType::EPathTypeExtSubDomain) {
                if (pathDescription.HasDomainDescription()) {
                    const auto& domainDescription(pathDescription.GetDomainDescription());
                    for (TTabletId tabletId : domainDescription.GetProcessingParams().GetCoordinators()) {
                        Tablets[tabletId] = NKikimrTabletBase::TTabletTypes::Coordinator;
                    }
                    for (TTabletId tabletId : domainDescription.GetProcessingParams().GetMediators()) {
                        Tablets[tabletId] = NKikimrTabletBase::TTabletTypes::Mediator;
                    }
                    if (domainDescription.GetProcessingParams().HasSchemeShard()) {
                        Tablets[domainDescription.GetProcessingParams().GetSchemeShard()] = NKikimrTabletBase::TTabletTypes::SchemeShard;
                    }
                    if (domainDescription.GetProcessingParams().HasHive()) {
                        Tablets[pathDescription.GetDomainDescription().GetProcessingParams().GetHive()] = NKikimrTabletBase::TTabletTypes::Hive;
                        HiveId = domainDescription.GetProcessingParams().GetHive();
                    } else {
                        HiveId = domainDescription.GetSharedHive();
                    }
                    if (domainDescription.GetProcessingParams().HasGraphShard()) {
                        Tablets[pathDescription.GetDomainDescription().GetProcessingParams().GetGraphShard()] = NKikimrTabletBase::TTabletTypes::GraphShard;
                    }
                    if (domainDescription.GetProcessingParams().HasSysViewProcessor()) {
                        Tablets[pathDescription.GetDomainDescription().GetProcessingParams().GetSysViewProcessor()] = NKikimrTabletBase::TTabletTypes::SysViewProcessor;
                    }
                    if (domainDescription.GetProcessingParams().HasStatisticsAggregator()) {
                        Tablets[pathDescription.GetDomainDescription().GetProcessingParams().GetStatisticsAggregator()] = NKikimrTabletBase::TTabletTypes::StatisticsAggregator;
                    }
                    if (domainDescription.GetProcessingParams().HasBackupController()) {
                        Tablets[pathDescription.GetDomainDescription().GetProcessingParams().GetBackupController()] = NKikimrTabletBase::TTabletTypes::BackupController;
                    }
                    TIntrusivePtr<TDomainsInfo> domains = AppData()->DomainsInfo;
                    auto* domain = domains->GetDomain();
                    if (describeResult->GetRecord().GetPathOwnerId() == domain->SchemeRoot && describeResult->GetRecord().GetPathId() == 1) {
                        Tablets[domain->SchemeRoot] = NKikimrTabletBase::TTabletTypes::SchemeShard;
                        Tablets[domains->GetHive()] = NKikimrTabletBase::TTabletTypes::Hive;
                        HiveId = domains->GetHive();
                        Tablets[MakeBSControllerID()] = NKikimrTabletBase::TTabletTypes::BSController;
                        Tablets[MakeConsoleID()] = NKikimrTabletBase::TTabletTypes::Console;
                        Tablets[MakeNodeBrokerID()] = NKikimrTabletBase::TTabletTypes::NodeBroker;
                        Tablets[MakeTenantSlotBrokerID()] = NKikimrTabletBase::TTabletTypes::TenantSlotBroker;
                        Tablets[MakeCmsID()] = NKikimrTabletBase::TTabletTypes::Cms;
                        for (TTabletId tabletId : domain->Coordinators) {
                            Tablets[tabletId] = NKikimrTabletBase::TTabletTypes::Coordinator;
                        }
                        for (TTabletId tabletId : domain->Mediators) {
                            Tablets[tabletId] = NKikimrTabletBase::TTabletTypes::Mediator;
                        }
                        for (TTabletId tabletId : domain->TxAllocators) {
                            Tablets[tabletId] = NKikimrTabletBase::TTabletTypes::TxAllocator;
                        }
                    }
                }
            }
        }
        if (!Tablets.empty()) {
            TBase::Bootstrap();
            for (auto tablet : Tablets) {
                Request->Record.AddFilterTabletId(tablet.first);
            }
        }
        RequestDone();
    }

    virtual void FilterResponse(NKikimrWhiteboard::TEvTabletStateResponse& response) override {
        if (!Tablets.empty()) {
            if (ReplyWithDeadTabletsInfo) {
                DeadTablets.reserve(Tablets.size());
                for (const auto& [tabletId, tabletType] : Tablets) {
                    DeadTablets.insert(tabletId);
                }
            }
            NKikimrWhiteboard::TEvTabletStateResponse result;
            for (const NKikimrWhiteboard::TTabletStateInfo& info : response.GetTabletStateInfo()) {
                auto tablet = Tablets.find(info.GetTabletId());
                if (tablet != Tablets.end()) {
                    auto tabletInfo = result.MutableTabletStateInfo()->Add();
                    tabletInfo->CopyFrom(info);
                    auto itKey = EndOfRangeKeyPrefix.find(info.GetTabletId());
                    if (itKey != EndOfRangeKeyPrefix.end()) {
                        tabletInfo->SetEndOfRangeKeyPrefix(itKey->second);
                    }
                    DeadTablets.erase(tablet->first);
                }
            }
            if (ReplyWithDeadTabletsInfo) {
                for (auto tabletId : DeadTablets) {
                    auto deadTablet = result.MutableTabletStateInfo()->Add();
                    deadTablet->SetTabletId(tabletId);
                    deadTablet->SetState(NKikimrWhiteboard::TTabletStateInfo::Dead);
                    deadTablet->SetType(Tablets[tabletId]);
                    deadTablet->SetHiveId(HiveId);
                    if (FilterTenantId) {
                        deadTablet->MutableTenantId()->CopyFrom(FilterTenantId);
                    }
                }
            }
            result.SetResponseTime(response.GetResponseTime());
            response = std::move(result);
        }
        auto& cont(*response.MutableTabletStateInfo());
        for (auto it = cont.begin(); it != cont.end();) {
            if (FilterTenantId && NKikimr::TSubDomainKey(it->GetTenantId()) != FilterTenantId) {
                it = cont.erase(it);
            } else {
                it->SetOverall(GetWhiteboardFlag(GetFlagFromTabletState(it->GetState())));
                ++it;
            }
        }
        TBase::FilterResponse(response);
    }

    STATEFN(StateRequestedLookup) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvStateStorage::TEvBoardInfo, Handle);
            cFunc(TEvents::TSystem::Wakeup, HandleTimeout);
        }
    }

    STATEFN(StateRequestedDescribe) {
        switch (ev->GetTypeRewrite()) {
            hFunc(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult, Handle);
            cFunc(TEvents::TSystem::Wakeup, HandleTimeout);
        }
    }
};

}
