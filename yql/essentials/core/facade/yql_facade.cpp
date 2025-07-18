#include "yql_facade.h"

#include <yql/essentials/core/yql_execution.h>
#include <yql/essentials/core/yql_expr_csee.h>
#include <yql/essentials/core/yql_expr_optimize.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <yql/essentials/core/yql_opt_rewrite_io.h>
#include <yql/essentials/core/yql_opt_proposed_by_data.h>
#include <yql/essentials/core/yql_gc_transformer.h>
#include <yql/essentials/core/type_ann/type_ann_expr.h>
#include <yql/essentials/core/services/yql_plan.h>
#include <yql/essentials/core/services/yql_eval_params.h>
#include <yql/essentials/core/langver/yql_core_langver.h>
#include <yql/essentials/sql/sql.h>
#include <yql/essentials/sql/v1/sql.h>
#include <yql/essentials/sql/v1/lexer/antlr4/lexer.h>
#include <yql/essentials/sql/v1/lexer/antlr4_ansi/lexer.h>
#include <yql/essentials/sql/v1/proto_parser/antlr4/proto_parser.h>
#include <yql/essentials/sql/v1/proto_parser/antlr4_ansi/proto_parser.h>
#include <yql/essentials/parser/pg_wrapper/interface/parser.h>
#include <yql/essentials/utils/log/context.h>
#include <yql/essentials/utils/log/profile.h>
#include <yql/essentials/utils/limiting_allocator.h>
#include <yql/essentials/core/services/yql_out_transformers.h>
#include <yql/essentials/core/extract_predicate/extract_predicate_dbg.h>
#include <yql/essentials/providers/common/provider/yql_provider_names.h>
#include <yql/essentials/providers/common/provider/yql_provider.h>
#include <yql/essentials/providers/common/gateways_utils/gateways_utils.h>
#include <yql/essentials/providers/common/udf_resolve/yql_simple_udf_resolver.h>
#include <yql/essentials/providers/common/udf_resolve/yql_outproc_udf_resolver.h>
#include <yql/essentials/providers/common/udf_resolve/yql_udf_resolver_with_index.h>
#include <yql/essentials/providers/common/udf_resolve/yql_udf_resolver_logger.h>
#include <yql/essentials/providers/common/arrow_resolve/yql_simple_arrow_resolver.h>
#include <yql/essentials/providers/common/config/yql_setting.h>
#include <yql/essentials/core/qplayer/udf_resolver/yql_qplayer_udf_resolver.h>

#include <library/cpp/yson/node/node_io.h>
#include <library/cpp/deprecated/split/split_iterator.h>
#include <library/cpp/yson/writer.h>
#include <library/cpp/string_utils/base64/base64.h>

#include <util/stream/file.h>
#include <util/stream/null.h>
#include <util/string/cast.h>
#include <util/string/join.h>
#include <util/string/split.h>
#include <util/generic/guid.h>
#include <util/system/rusage.h>
#include <util/generic/yexception.h>


using namespace NThreading;

namespace NYql {

namespace {

const size_t DEFAULT_AST_BUF_SIZE = 1024;
const size_t DEFAULT_PLAN_BUF_SIZE = 1024;
const TString FacadeComponent = "Facade";
const TString SourceCodeLabel = "SourceCode";
const TString LangVerLabel = "LangVer";
const TString GatewaysLabel = "Gateways";
const TString ParametersLabel = "Parameters";
const TString TranslationLabel = "Translation";
const TString StaticUserFilesLabel = "UserFiles";
const TString DynamicUserFilesLabel = "DynamicUserFiles";
const TString StaticCredentialsLabel = "Credentials";
const TString DynamicCredentialsLabel = "DynamicCredentials";

class TUrlLoader : public IUrlLoader {
public:
    TUrlLoader(TFileStoragePtr storage)
        : Storage_(storage)
    {}

    TString Load(const TString& url, const TString& token) override {
        auto file = Storage_->PutUrl(url, token);
        return TFileInput(file->GetPath()).ReadAll();
    }

private:
    TFileStoragePtr Storage_;
};

template <typename... Params1, typename... Params2>
TProgram::TStatus SyncExecution(
        TProgram* program,
        TProgram::TFutureStatus (TProgram::*method)(Params1...),
        Params2&&... params) {
    TProgram::TFutureStatus future =
            (program->*method)(std::forward<Params2>(params)...);
    YQL_ENSURE(future.Initialized());
    future.Wait();
    HandleFutureException(future);

    TProgram::TStatus status = future.GetValue();
    while (status == TProgram::TStatus::Async) {
        auto continueFuture = program->ContinueAsync();
        continueFuture.Wait();
        HandleFutureException(continueFuture);
        status = continueFuture.GetValue();
    }

    if (status == TProgram::TStatus::Error) {
        program->Print(program->ExprStream(), program->PlanStream());
    }

    return status;
}

std::function<TString(const TString&, const TString&)> BuildDefaultTokenResolver(TCredentials::TPtr credentials) {
    return [credentials](const TString& /*url*/, const TString& alias) -> TString {
        if (alias) {
            if (auto cred = credentials->FindCredential(TString("default_").append(alias))) {
                return cred->Content;
            }
        }
        return {};
    };
}

std::function<TString(const TString&, const TString&)> BuildCompositeTokenResolver(TVector<std::function<TString(const TString&, const TString&)>>&& children) {
    if (children.empty()) {
        return {};
    }

    if (children.size() == 1) {
        return std::move(children[0]);
    }

    return [children = std::move(children)](const TString& url, const TString& alias) -> TString {
        for (auto& c : children) {
            if (auto r = c(url, alias)) {
                return r;
            }
        }

        return {};
    };
}

} // namspace

///////////////////////////////////////////////////////////////////////////////
// TProgramFactory
///////////////////////////////////////////////////////////////////////////////
TProgramFactory::TProgramFactory(
    bool useRepeatableRandomAndTimeProviders,
    const NKikimr::NMiniKQL::IFunctionRegistry* functionRegistry,
    ui64 nextUniqueId,
    const TVector<TDataProviderInitializer>& dataProvidersInit,
    const TString& runner)
    : UseRepeatableRandomAndTimeProviders_(useRepeatableRandomAndTimeProviders)
    , FunctionRegistry_(functionRegistry)
    , NextUniqueId_(nextUniqueId)
    , DataProvidersInit_(dataProvidersInit)
    , Credentials_(MakeIntrusive<TCredentials>())
    , GatewaysConfig_(nullptr)
    , Runner_(runner)
    , ArrowResolver_(MakeSimpleArrowResolver(*functionRegistry))
{
}

void TProgramFactory::UnrepeatableRandom() {
    UseUnrepeatableRandom_ = true;
}

void TProgramFactory::EnableRangeComputeFor() {
    EnableRangeComputeFor_ = true;
}

void TProgramFactory::SetLanguageVersion(TLangVersion version) {
    LangVer_ = version;
}

void TProgramFactory::SetMaxLanguageVersion(TLangVersion version) {
    MaxLangVer_ = version;
}

void TProgramFactory::SetVolatileResults() {
    VolatileResults_ = true;
}

void TProgramFactory::AddUserDataTable(const TUserDataTable& userDataTable) {
    for (const auto& p : userDataTable) {
        if (!UserDataTable_.emplace(p).second) {
            ythrow yexception() << "UserDataTable already has user data block with key " << p.first;
        }
    }
}

void TProgramFactory::SetCredentials(TCredentials::TPtr credentials) {
    Credentials_ = std::move(credentials);
}

void TProgramFactory::SetGatewaysConfig(const TGatewaysConfig* gatewaysConfig) {
    GatewaysConfig_ = gatewaysConfig;
}

void TProgramFactory::SetModules(IModuleResolver::TPtr modules) {
    Modules_ = modules;
}

void TProgramFactory::SetUdfResolver(IUdfResolver::TPtr udfResolver) {
    UdfResolver_ = udfResolver;
}

void TProgramFactory::SetUdfIndex(TUdfIndex::TPtr udfIndex, TUdfIndexPackageSet::TPtr udfIndexPackageSet) {
    UdfIndex_ = std::move(udfIndex);
    UdfIndexPackageSet_ = std::move(udfIndexPackageSet);
}

void TProgramFactory::SetFileStorage(TFileStoragePtr fileStorage) {
    FileStorage_ = std::move(fileStorage);
}

void TProgramFactory::SetUrlPreprocessing(IUrlPreprocessing::TPtr urlPreprocessing) {
    UrlPreprocessing_ = std::move(urlPreprocessing);
}

void TProgramFactory::SetArrowResolver(IArrowResolver::TPtr arrowResolver) {
    ArrowResolver_ = arrowResolver;
}

void TProgramFactory::SetUdfResolverLogfile(const TString& path) {
    UdfResolverLogfile_ = path;
}

void TProgramFactory::SetUrlListerManager(IUrlListerManagerPtr urlListerManager) {
    UrlListerManager_ = std::move(urlListerManager);
}

TProgramPtr TProgramFactory::Create(
        const TFile& file,
        const TString& sessionId,
        const TQContext& qContext,
        TMaybe<TString> gatewaysForMerge)
{
    TString sourceCode = TFileInput(file).ReadAll();
    return Create(file.GetName(), sourceCode, sessionId, EHiddenMode::Disable, qContext, gatewaysForMerge);
}

TProgramPtr TProgramFactory::Create(
        const TString& filename,
        const TString& sourceCode,
        const TString& sessionId,
        EHiddenMode hiddenMode,
        const TQContext& qContext,
        TMaybe<TString> gatewaysForMerge)
{
    auto randomProvider = UseRepeatableRandomAndTimeProviders_ && !UseUnrepeatableRandom_ && hiddenMode == EHiddenMode::Disable ?
        CreateDeterministicRandomProvider(1) : CreateDefaultRandomProvider();
    auto timeProvider = UseRepeatableRandomAndTimeProviders_ ?
        CreateDeterministicTimeProvider(10000000) : CreateDefaultTimeProvider();

    TUdfIndex::TPtr udfIndex = UdfIndex_ ? UdfIndex_->Clone() : nullptr;
    TUdfIndexPackageSet::TPtr udfIndexPackageSet = (UdfIndexPackageSet_ && hiddenMode == EHiddenMode::Disable) ? UdfIndexPackageSet_->Clone() : nullptr;
    IModuleResolver::TPtr moduleResolver = Modules_ ? Modules_->CreateMutableChild() : nullptr;
    IUrlListerManagerPtr urlListerManager = UrlListerManager_ ? UrlListerManager_->Clone() : nullptr;

    auto udfResolver = UdfResolver_;

    if (UdfResolverLogfile_) {
        udfResolver = NCommon::CreateUdfResolverDecoratorWithLogger(FunctionRegistry_, udfResolver, *UdfResolverLogfile_, sessionId);
    }

    if (udfIndex) {
        udfResolver = NCommon::CreateUdfResolverWithIndex(udfIndex, udfResolver, FileStorage_);
    }

    // make UserDataTable_ copy here
    return new TProgram(FunctionRegistry_, randomProvider, timeProvider, NextUniqueId_, DataProvidersInit_,
        LangVer_, MaxLangVer_, VolatileResults_, UserDataTable_, Credentials_, moduleResolver, urlListerManager,
        udfResolver, udfIndex, udfIndexPackageSet, FileStorage_, UrlPreprocessing_,
        GatewaysConfig_, filename, sourceCode, sessionId, Runner_, EnableRangeComputeFor_, ArrowResolver_, hiddenMode,
        qContext, gatewaysForMerge);
}

///////////////////////////////////////////////////////////////////////////////
// TProgram
///////////////////////////////////////////////////////////////////////////////
TProgram::TProgram(
        const NKikimr::NMiniKQL::IFunctionRegistry* functionRegistry,
        const TIntrusivePtr<IRandomProvider> randomProvider,
        const TIntrusivePtr<ITimeProvider> timeProvider,
        ui64 nextUniqueId,
        const TVector<TDataProviderInitializer>& dataProvidersInit,
        TLangVersion langVer,
        TLangVersion maxLangVer,
        bool volatileResults,
        const TUserDataTable& userDataTable,
        const TCredentials::TPtr& credentials,
        const IModuleResolver::TPtr& modules,
        const IUrlListerManagerPtr& urlListerManager,
        const IUdfResolver::TPtr& udfResolver,
        const TUdfIndex::TPtr& udfIndex,
        const TUdfIndexPackageSet::TPtr& udfIndexPackageSet,
        const TFileStoragePtr& fileStorage,
        const IUrlPreprocessing::TPtr& urlPreprocessing,
        const TGatewaysConfig* gatewaysConfig,
        const TString& filename,
        const TString& sourceCode,
        const TString& sessionId,
        const TString& runner,
        bool enableRangeComputeFor,
        const IArrowResolver::TPtr& arrowResolver,
        EHiddenMode hiddenMode,
        const TQContext& qContext,
        TMaybe<TString> gatewaysForMerge
    )
    : FunctionRegistry_(functionRegistry)
    , RandomProvider_(randomProvider)
    , TimeProvider_(timeProvider)
    , NextUniqueId_(nextUniqueId)
    , AstRoot_(nullptr)
    , Modules_(modules)
    , DataProvidersInit_(dataProvidersInit)
    , LangVer_(langVer)
    , MaxLangVer_(maxLangVer)
    , VolatileResults_(volatileResults)
    , Credentials_(MakeIntrusive<NYql::TCredentials>(*credentials))
    , UrlListerManager_(urlListerManager)
    , UdfResolver_(udfResolver)
    , UdfIndex_(udfIndex)
    , UdfIndexPackageSet_(udfIndexPackageSet)
    , FileStorage_(fileStorage)
    , SavedUserDataTable_(userDataTable)
    , GatewaysConfig_(gatewaysConfig)
    , Filename_(filename)
    , SourceCode_(sourceCode)
    , SourceSyntax_(ESourceSyntax::Unknown)
    , SyntaxVersion_(0)
    , ExprRoot_(nullptr)
    , SessionId_(sessionId)
    , ResultType_(IDataProvider::EResultFormat::Yson)
    , ResultFormat_(NYson::EYsonFormat::Binary)
    , OutputFormat_(NYson::EYsonFormat::Pretty)
    , EnableRangeComputeFor_(enableRangeComputeFor)
    , ArrowResolver_(arrowResolver)
    , HiddenMode_(hiddenMode)
    , QContext_(qContext)
    , GatewaysForMerge_(gatewaysForMerge)
{
    if (SessionId_.empty()) {
        SessionId_ = CreateGuidAsString();
    }

    if (QContext_.CanWrite()) {
        NYT::TNode credListNode = NYT::TNode::CreateList();
        Credentials_->ForEach([&](const TString name, const TCredential& cred) {
            credListNode.Add(NYT::TNode()
                ("Name", name)
                ("Category", cred.Category)
                ("Subcategory", cred.Subcategory));
        });

        auto credList = NYT::NodeToYsonString(credListNode, NYT::NYson::EYsonFormat::Binary);
        QContext_.GetWriter()->Put({FacadeComponent, StaticCredentialsLabel}, credList).GetValueSync();
    } else if (QContext_.CanRead()) {
        Credentials_ = MakeIntrusive<TCredentials>();
        Credentials_->SetUserCredentials({
            .OauthToken = "REPLAY_OAUTH",
            .BlackboxSessionIdCookie = "REPLAY_SESSIONID"
        });

        for (const auto& label : {StaticCredentialsLabel, DynamicCredentialsLabel}) {
            auto item = QContext_.GetReader()->Get({FacadeComponent, label}).GetValueSync();
            if (item) {
                auto node = NYT::NodeFromYsonString(item->Value);
                for (const auto& c : node.AsList()) {
                    Credentials_->AddCredential(c["Name"].AsString(), TCredential(
                        c["Category"].AsString(),c["Subcategory"].AsString(),"REPLAY"));
                }
            }
        }
    }

    if (QContext_.CanWrite() && !SavedUserDataTable_.empty()) {
        NYT::TNode userFilesNode = NYT::TNode::CreateList();
        for (const auto& p : SavedUserDataTable_) {
            userFilesNode.Add(p.first.Alias());
        }

        auto userFiles = NYT::NodeToYsonString(userFilesNode, NYT::NYson::EYsonFormat::Binary);
        QContext_.GetWriter()->Put({FacadeComponent, StaticUserFilesLabel}, userFiles).GetValueSync();
    } else if (QContext_.CanRead()) {
        SavedUserDataTable_.clear();
        for (const auto& label : {StaticUserFilesLabel, DynamicUserFilesLabel}) {
            auto item = QContext_.GetReader()->Get({FacadeComponent, label}).GetValueSync();
            if (item) {
                auto node = NYT::NodeFromYsonString(item->Value);
                for (const auto& alias : node.AsList()) {
                    TUserDataBlock block;
                    block.Type = EUserDataType::RAW_INLINE_DATA;
                    YQL_ENSURE(SavedUserDataTable_.emplace(TUserDataKey::File(alias.AsString()), block).second);
                }
            }
        }
    }

    UserDataStorage_ = MakeIntrusive<TUserDataStorage>(fileStorage, SavedUserDataTable_, udfResolver, udfIndex);
    if (auto modules = dynamic_cast<TModuleResolver*>(Modules_.get())) {
        modules->AttachUserData(UserDataStorage_);
        modules->SetUrlLoader(new TUrlLoader(FileStorage_));
        modules->SetCredentials(Credentials_);
        if (QContext_) {
            modules->SetQContext(QContext_);
        }
    }

    if (UrlListerManager_) {
        UrlListerManager_->SetCredentials(Credentials_);
        UrlListerManager_->SetUrlPreprocessing(urlPreprocessing);
    }

    OperationOptions_.Runner = runner;
    UserDataStorage_->SetUrlPreprocessor(urlPreprocessing);

    if (QContext_) {
        UdfResolver_ = NCommon::WrapUdfResolverWithQContext(UdfResolver_, QContext_);
        if (QContext_.CanRead()) {
            auto item = QContext_.GetReader()->Get({FacadeComponent, GatewaysLabel}).GetValueSync();
            if (item) {
                YQL_ENSURE(LoadedGatewaysConfig_.ParseFromString(item->Value));
                if (GatewaysForMerge_) {
                    YQL_ENSURE(NProtoBuf::TextFormat::MergeFromString(*GatewaysForMerge_, &LoadedGatewaysConfig_));
                }
                THashMap<TString, TString> clusterMapping;
                GetClusterMappingFromGateways(LoadedGatewaysConfig_, clusterMapping);
                auto sqlFlags = ExtractSqlFlags(LoadedGatewaysConfig_);
                if (auto modules = dynamic_cast<TModuleResolver*>(Modules_.get())) {
                    modules->SetClusterMapping(clusterMapping);
                    modules->SetSqlFlags(sqlFlags);
                }

                GatewaysConfig_ = &LoadedGatewaysConfig_;
            }
        } else if (QContext_.CanWrite() && GatewaysConfig_) {
            TGatewaysConfig cleaned;
            if (GatewaysConfig_->HasYt()) {
                cleaned.MutableYt()->CopyFrom(GatewaysConfig_->GetYt());
            }

            if (GatewaysConfig_->HasFs()) {
                cleaned.MutableFs()->CopyFrom(GatewaysConfig_->GetFs());
            }

            if (GatewaysConfig_->HasYqlCore()) {
                cleaned.MutableYqlCore()->CopyFrom(GatewaysConfig_->GetYqlCore());
            }

            if (GatewaysConfig_->HasSqlCore()) {
                cleaned.MutableSqlCore()->CopyFrom(GatewaysConfig_->GetSqlCore());
            }

            if (GatewaysConfig_->HasDq()) {
                cleaned.MutableDq()->CopyFrom(GatewaysConfig_->GetDq());
            }

            auto data = cleaned.SerializeAsString();
            QContext_.GetWriter()->Put({FacadeComponent, GatewaysLabel}, data).GetValueSync();
        }

        if (QContext_.CanRead()) {
            auto item = QContext_.GetReader()->Get({FacadeComponent, ParametersLabel}).GetValueSync();
            if (item) {
                SetParametersYson(item->Value);
            }
        }
    }
}

TProgram::~TProgram() {
    try {
        CloseLastSession().GetValueSync();
        // stop all non complete execution before deleting TExprCtx
        with_lock (DataProvidersLock_) {
            DataProviders_.clear();
        }
    } catch (...) {
        Cerr << CurrentExceptionMessage() << Endl;
    }
}

void TProgram::ConfigureYsonResultFormat(NYson::EYsonFormat format) {
    ResultFormat_ = format;
    OutputFormat_ = format;
}

void TProgram::SetValidateOptions(NUdf::EValidateMode validateMode) {
    Y_ENSURE(!TypeCtx_, "TypeCtx_ already created");
    ValidateMode_ = validateMode;
}

void TProgram::SetDisableNativeUdfSupport(bool disable) {
    Y_ENSURE(!TypeCtx_, "TypeCtx_ already created");
    DisableNativeUdfSupport_ = disable;
}

void TProgram::SetUseTableMetaFromGraph(bool use) {
    Y_ENSURE(!TypeCtx_, "TypeCtx_ already created");
    UseTableMetaFromGraph_ = use;
}

TTypeAnnotationContextPtr TProgram::GetAnnotationContext() const {
    Y_ENSURE(TypeCtx_, "TypeCtx_ is not created");
    return TypeCtx_;
}

TTypeAnnotationContextPtr TProgram::ProvideAnnotationContext(const TString& username) {
    if (!TypeCtx_) {
        TypeCtx_ = BuildTypeAnnotationContext(username);
        TypeCtx_->OperationOptions = OperationOptions_;
        TypeCtx_->ValidateMode = ValidateMode_;
        TypeCtx_->DisableNativeUdfSupport = DisableNativeUdfSupport_;
        TypeCtx_->UseTableMetaFromGraph = UseTableMetaFromGraph_;
    }

    return TypeCtx_;
}

IPlanBuilder& TProgram::GetPlanBuilder() {
    if (!PlanBuilder_) {
        PlanBuilder_ = CreatePlanBuilder(*GetAnnotationContext());
    }

    return *PlanBuilder_;
}

void TProgram::SetParametersYson(const TString& parameters) {
    Y_ENSURE(!TypeCtx_, "TypeCtx_ already created");
    NYT::TNode node;
    try {
        try {
            node = NYT::NodeFromYsonString(parameters);
        } catch (const std::exception& e) {
            throw TErrorException(0) << "Invalid parameters: " << e.what();
        }

        if (!node.IsMap()) {
            throw TErrorException(0) << "Invalid parameters: expected Map at first level";
        }

        for (const auto& x : node.AsMap()) {
            if (!x.second.IsMap()) {
                throw TErrorException(0) << "Invalid parameters: expected Map at second level";
            }

            if (!x.second.HasKey("Data")) {
                throw TErrorException(0) << "Invalid parameters: expected Data key";
            }

            if (x.second.Size() != 1) {
                throw TErrorException(0) << "Invalid parameters: expected Map with single element";
            }
        }
    } catch (const std::exception& e) {
        ParametersIssue_ = ExceptionToIssue(e);
        return;
    }

    OperationOptions_.ParametersYson = node;
    if (auto modules = dynamic_cast<TModuleResolver*>(Modules_.get())) {
        modules->SetParameters(node);
    }

    if (UrlListerManager_) {
        UrlListerManager_->SetParameters(node);
    }

    if (QContext_.CanWrite()) {
        QContext_.GetWriter()->Put({FacadeComponent, ParametersLabel}, parameters).GetValueSync();
    }
}

bool TProgram::ExtractQueryParametersMetadata() {
    auto& types = *GetAnnotationContext();
    NYT::TNode params = NYT::TNode::CreateMap();
    Y_ENSURE(ExprCtx_);
    if (!ExtractParametersMetaAsYson(ExprRoot_, types, *ExprCtx_, params)) {
        return false;
    }

    ExtractedQueryParametersMetadataYson_ = NYT::NodeToYsonString(params, ResultFormat_);
    return true;
}

bool TProgram::FillParseResult(NYql::TAstParseResult&& astRes, NYql::TWarningRules* warningRules) {
    if (!astRes.Issues.Empty()) {
        if (!ExprCtx_) {
            ExprCtx_.Reset(new TExprContext(NextUniqueId_));
        }
        auto& iManager = ExprCtx_->IssueManager;
        if (warningRules) {
            for (auto warningRule: *warningRules) {
                iManager.AddWarningRule(warningRule);
            }
        }
        iManager.AddScope([this]() {
            TIssuePtr issueHolder = new TIssue();
            issueHolder->SetMessage(TStringBuilder() << "Parse " << SourceSyntax_);
            issueHolder->Severity = TSeverityIds::S_INFO;
            return issueHolder;
        });
        for (auto issue: astRes.Issues) {
            iManager.RaiseWarning(issue);
        }
        iManager.LeaveScope();
    }
    if (!astRes.IsOk()) {
        return false;
    }
    AstRoot_ = astRes.Root;
    AstPool_ = std::move(astRes.Pool);
    return true;
}

TString TProgram::GetSessionId() const {
    with_lock(SessionIdLock_) {
        return SessionId_;
    }
}

void TProgram::SetLanguageVersion(TLangVersion version) {
    LangVer_ = version;
}

void TProgram::SetMaxLanguageVersion(TLangVersion version) {
    MaxLangVer_ = version;
}

void TProgram::SetVolatileResults() {
    VolatileResults_ = true;
}

void TProgram::AddCredentials(const TVector<std::pair<TString, TCredential>>& credentials) {
    Y_ENSURE(!TypeCtx_, "TypeCtx_ already created");

    if (QContext_.CanWrite()) {
        NYT::TNode credListNode = NYT::TNode::CreateList();
        for (const auto& c : credentials) {
            credListNode.Add(NYT::TNode()
                ("Name", c.first)
                ("Category", c.second.Category)
                ("Subcategory", c.second.Subcategory));
        }

        auto credList = NYT::NodeToYsonString(credListNode, NYT::NYson::EYsonFormat::Binary);
        QContext_.GetWriter()->Put({FacadeComponent, DynamicCredentialsLabel}, credList).GetValueSync();
    }

    for (const auto& credential : credentials) {
        Credentials_->AddCredential(credential.first, credential.second);
    }

    if (auto modules = dynamic_cast<TModuleResolver*>(Modules_.get())) {
        modules->SetCredentials(Credentials_);
    }
    if (UrlListerManager_) {
        UrlListerManager_->SetCredentials(Credentials_);
    }
}

void TProgram::ClearCredentials() {
    Y_ENSURE(!TypeCtx_, "TypeCtx_ already created");

    Credentials_ = MakeIntrusive<TCredentials>();

    if (auto modules = dynamic_cast<TModuleResolver*>(Modules_.get())) {
        modules->SetCredentials(Credentials_);
    }
    if (UrlListerManager_) {
        UrlListerManager_->SetCredentials(Credentials_);
    }
}

void TProgram::AddUserDataTable(const TUserDataTable& userDataTable) {
    for (const auto& p : userDataTable) {
        if (!SavedUserDataTable_.emplace(p).second) {
            ythrow yexception() << "UserDataTable already has user data block with key " << p.first;
        }
        UserDataStorage_->AddUserDataBlock(p.first, p.second);
    }

    if (QContext_.CanWrite()) {
        NYT::TNode userFilesNode;
        for (const auto& p : userDataTable) {
            userFilesNode.Add(p.first.Alias());
        }

        auto userFiles = NYT::NodeToYsonString(userFilesNode, NYT::NYson::EYsonFormat::Binary);
        QContext_.GetWriter()->Put({FacadeComponent, DynamicUserFilesLabel}, userFiles).GetValueSync();
    }
}

void TProgram::HandleSourceCode(TString& sourceCode) {
    if (QContext_.CanWrite()) {
        QContext_.GetWriter()->Put({FacadeComponent, SourceCodeLabel}, sourceCode).GetValueSync();
    } else if (QContext_.CanRead()) {
        auto loaded = QContext_.GetReader()->Get({FacadeComponent, SourceCodeLabel}).GetValueSync();
        Y_ENSURE(loaded.Defined(), "No source code");
        sourceCode = loaded->Value;
    }
}

namespace {

THashSet<TString> ExtractSqlFlags(const NYT::TNode& dataNode) {
    THashSet<TString> result;
    for (const auto& f : dataNode["SqlFlags"].AsList()) {
        result.insert(f.AsString());
    }
    return result;
}

} // namespace

void UpdateSqlFlagsFromQContext(const TQContext& qContext, THashSet<TString>& flags) {
    if (qContext.CanRead()) {
        auto loaded = qContext.GetReader()->Get({FacadeComponent, TranslationLabel}).GetValueSync();
        if (!loaded) {
            return;
        }

        auto dataNode = NYT::NodeFromYsonString(loaded->Value);
        flags = ExtractSqlFlags(dataNode);
    }
}

void TProgram::HandleTranslationSettings(NSQLTranslation::TTranslationSettings& loadedSettings,
    NSQLTranslation::TTranslationSettings*& currentSettings)
{
    if (QContext_.CanWrite()) {
        auto clusterMappingsNode = NYT::TNode::CreateMap();
        for (const auto& c : currentSettings->ClusterMapping) {
            clusterMappingsNode(c.first, c.second);
        }

        auto sqlFlagsNode = NYT::TNode::CreateList();
        for (const auto& f : currentSettings->Flags) {
            sqlFlagsNode.Add(f);
        }

        auto dataNode = NYT::TNode()
            ("ClusterMapping", clusterMappingsNode)
            ("V0Behavior", ui64(currentSettings->V0Behavior))
            ("V0WarnAsError", currentSettings->V0WarnAsError->Allow())
            ("DqDefaultAuto", currentSettings->DqDefaultAuto->Allow())
            ("BlockDefaultAuto", currentSettings->BlockDefaultAuto->Allow())
            ("SqlFlags", sqlFlagsNode);

        auto data = NYT::NodeToYsonString(dataNode, NYT::NYson::EYsonFormat::Binary);
        QContext_.GetWriter()->Put({FacadeComponent, TranslationLabel}, data).GetValueSync();
    } else if (QContext_.CanRead()) {
        auto loaded = QContext_.GetReader()->Get({FacadeComponent, TranslationLabel}).GetValueSync();
        if (!loaded) {
            return;
        }

        auto dataNode = NYT::NodeFromYsonString(loaded->Value);
        loadedSettings.ClusterMapping.clear();
        for (const auto& c : dataNode["ClusterMapping"].AsMap()) {
            loadedSettings.ClusterMapping[c.first] = c.second.AsString();
        }

        loadedSettings.Flags = ExtractSqlFlags(dataNode);
        loadedSettings.V0Behavior = (NSQLTranslation::EV0Behavior)dataNode["V0Behavior"].AsUint64();
        loadedSettings.V0WarnAsError = NSQLTranslation::ISqlFeaturePolicy::Make(dataNode["V0WarnAsError"].AsBool());
        loadedSettings.DqDefaultAuto = NSQLTranslation::ISqlFeaturePolicy::Make(dataNode["DqDefaultAuto"].AsBool());
        loadedSettings.BlockDefaultAuto = NSQLTranslation::ISqlFeaturePolicy::Make(dataNode["BlockDefaultAuto"].AsBool());
        loadedSettings.IsReplay = true;
        currentSettings = &loadedSettings;
    }
}

bool TProgram::CheckParameters() {
    if (ParametersIssue_) {
        if (!ExprCtx_) {
            ExprCtx_.Reset(new TExprContext(NextUniqueId_));
        }

        ExprCtx_->AddError(*ParametersIssue_);
        return false;
    }

    return true;
}

bool TProgram::ValidateLangVersion() {
    if (QContext_.CanRead()) {
        auto loaded = QContext_.GetReader()->Get({FacadeComponent, LangVerLabel}).GetValueSync();
        if (loaded.Defined()) {
            LangVer_ = FromString<TLangVersion>(loaded->Value);
        } else {
            LangVer_ = UnknownLangVersion;
        }

        return true;
    }

    if (QContext_.CanWrite()) {
        QContext_.GetWriter()->Put({FacadeComponent, LangVerLabel}, ToString(LangVer_)).GetValueSync();
    }

    TMaybe<TIssue> issue;
    auto ret = CheckLangVersion(LangVer_, MaxLangVer_, issue);
    if (issue) {
        if (!ExprCtx_) {
            ExprCtx_.Reset(new TExprContext(NextUniqueId_));
        }

        ExprCtx_->AddError(*issue);
    }

    return ret;
}

bool TProgram::ParseYql() {
    YQL_PROFILE_FUNC(TRACE);
    if (!ValidateLangVersion()) {
        return false;
    }

    if (!CheckParameters()) {
        return false;
    }

    YQL_ENSURE(SourceSyntax_ == ESourceSyntax::Unknown);
    SourceSyntax_ = ESourceSyntax::Yql;
    SyntaxVersion_ = 1;
    auto sourceCode = SourceCode_;
    HandleSourceCode(sourceCode);
    return FillParseResult(ParseAst(sourceCode));
}

bool TProgram::ParseSql() {
    YQL_PROFILE_FUNC(TRACE);

    static const THashMap<TString, TString> clusters = {
        { "plato", TString(YtProviderName) }
    };

    NSQLTranslation::TTranslationSettings settings;
    settings.ClusterMapping = clusters;
    return ParseSql(settings);
}

bool TProgram::ParseSql(const NSQLTranslation::TTranslationSettings& settings)
{
    YQL_PROFILE_FUNC(TRACE);
    if (!ValidateLangVersion()) {
        return false;
    }

    if (!CheckParameters()) {
        return false;
    }

    YQL_ENSURE(SourceSyntax_ == ESourceSyntax::Unknown);
    SourceSyntax_ = ESourceSyntax::Sql;
    SyntaxVersion_ = settings.SyntaxVersion;
    NYql::TWarningRules warningRules;
    auto sourceCode = SourceCode_;
    HandleSourceCode(sourceCode);
    NSQLTranslation::TTranslationSettings outerSettings = settings;
    NSQLTranslation::TTranslationSettings* currentSettings = &outerSettings;
    NSQLTranslation::TTranslationSettings loadedSettings;
    loadedSettings.PgParser = settings.PgParser;
    if (QContext_) {
        HandleTranslationSettings(loadedSettings, currentSettings);
    }

    currentSettings->EmitReadsForExists = true;
    currentSettings->LangVer = LangVer_;

    NSQLTranslationV1::TLexers lexers;
    lexers.Antlr4 = NSQLTranslationV1::MakeAntlr4LexerFactory();
    lexers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiLexerFactory();
    NSQLTranslationV1::TParsers parsers;
    parsers.Antlr4 = NSQLTranslationV1::MakeAntlr4ParserFactory();
    parsers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiParserFactory();

    NSQLTranslation::TTranslators translators(
        nullptr,
        NSQLTranslationV1::MakeTranslator(lexers, parsers),
        NSQLTranslationPG::MakeTranslator()
    );

    return FillParseResult(SqlToYql(translators, sourceCode, *currentSettings, &warningRules), &warningRules);
}

bool TProgram::Compile(const TString& username, bool skipLibraries) {
    YQL_PROFILE_FUNC(TRACE);

    Y_ENSURE(AstRoot_, "Program not parsed yet");
    if (!ExprCtx_) {
        ExprCtx_.Reset(new TExprContext(NextUniqueId_));
    }

    if (!ProvideAnnotationContext(username)->Initialize(*ExprCtx_)) {
        return false;
    }
    TypeCtx_->IsReadOnly = true;
    if (!skipLibraries && Modules_.get()) {
        auto libs = UserDataStorage_->GetLibraries();
        for (auto lib : libs) {
            if (!Modules_->AddFromFile(lib, *ExprCtx_, SyntaxVersion_, 0)) {
                return false;
            }
        }
    }

    if (!CompileExpr(
        *AstRoot_, ExprRoot_, *ExprCtx_,
        skipLibraries ? nullptr : Modules_.get(),
        skipLibraries ? nullptr : UrlListerManager_.Get(), 0, SyntaxVersion_
    )) {
        return false;
    }

    return true;
}

bool TProgram::CollectUsedClusters() {
    using namespace NNodes;

    if (!UsedClusters_) {
        UsedClusters_.ConstructInPlace();
        UsedProviders_.ConstructInPlace();

        auto& typesCtx = *GetAnnotationContext();
        auto& ctx = *ExprCtx_;
        auto& usedClusters = *UsedClusters_;
        auto& usedProviders = *UsedProviders_;
        bool hasErrors = false;

        VisitExpr(ExprRoot_, [&typesCtx, &ctx, &usedClusters, &usedProviders, &hasErrors](const TExprNode::TPtr& node) {
            if (auto dsNode = TMaybeNode<TCoDataSource>(node)) {
                auto datasource = typesCtx.DataSourceMap.FindPtr(dsNode.Cast().Category());
                YQL_ENSURE(datasource, "Unknown DataSource: " << dsNode.Cast().Category().Value());

                TMaybe<TString> cluster;
                if (!(*datasource)->ValidateParameters(*node, ctx, cluster)) {
                    hasErrors = true;
                    return false;
                }

                usedProviders.insert(TString(dsNode.Cast().Category().Value()));
                if (cluster && *cluster != NCommon::ALL_CLUSTERS) {
                    usedClusters.insert(*cluster);
                }
            }

            if (auto dsNode = TMaybeNode<TCoDataSink>(node)) {
                auto datasink = typesCtx.DataSinkMap.FindPtr(dsNode.Cast().Category());
                YQL_ENSURE(datasink, "Unknown DataSink: " << dsNode.Cast().Category().Value());

                TMaybe<TString> cluster;
                if (!(*datasink)->ValidateParameters(*node, ctx, cluster)) {
                    hasErrors = true;
                    return false;
                }

                usedProviders.insert(TString(dsNode.Cast().Category().Value()));
                if (cluster) {
                    usedClusters.insert(*cluster);
                }
            }

            return true;
        });

        if (hasErrors) {
            UsedClusters_ = Nothing();
            UsedProviders_ = Nothing();
            return false;
        }
    }

    return true;
}

TProgram::TStatus TProgram::Discover(const TString& username) {
    YQL_PROFILE_FUNC(TRACE);
    auto m = &TProgram::DiscoverAsync;
    return SyncExecution(this, m, username);
}

TProgram::TFutureStatus TProgram::DiscoverAsync(const TString& username) {
    if (!ProvideAnnotationContext(username)->Initialize(*ExprCtx_) || !CollectUsedClusters()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }
    TypeCtx_->DiscoveryMode = true;

    Y_ENSURE(ExprRoot_, "Program not compiled yet");

    Transformer_ = TTransformationPipeline(TypeCtx_)
            .AddServiceTransformers()
            .AddParametersEvaluation(*FunctionRegistry_)
            .AddPreTypeAnnotation()
            .AddExpressionEvaluation(*FunctionRegistry_)
            .AddPreIOAnnotation()
            .Build();

    TFuture<void> openSession = OpenSession(username);
    if (!openSession.Initialized()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }

    return openSession.Apply([this](const TFuture<void>& f) {
        YQL_LOG_CTX_ROOT_SESSION_SCOPE(GetSessionId());
        try {
            f.GetValue();
        } catch (const std::exception& e) {
            YQL_LOG(ERROR) << "OpenSession error: " << e.what();
            ExprCtx_->IssueManager.RaiseIssue(ExceptionToIssue(e));
            return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
        }
        return AsyncTransform(*Transformer_, ExprRoot_, *ExprCtx_, false);
    });
}

TProgram::TStatus TProgram::Lineage(const TString& username, IOutputStream* traceOut, IOutputStream* exprOut, bool withTypes) {
    YQL_PROFILE_FUNC(TRACE);
    auto m = &TProgram::LineageAsync;
    return SyncExecution(this, m, username, traceOut, exprOut, withTypes);
}

TProgram::TFutureStatus TProgram::LineageAsync(const TString& username, IOutputStream* traceOut, IOutputStream* exprOut, bool withTypes) {
    if (!ProvideAnnotationContext(username)->Initialize(*ExprCtx_) || !CollectUsedClusters()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }
    TypeCtx_->IsReadOnly = true;

    Y_ENSURE(ExprRoot_, "Program not compiled yet");

    ExprStream_ = exprOut;
    Transformer_ = TTransformationPipeline(TypeCtx_)
        .AddServiceTransformers()
        .AddParametersEvaluation(*FunctionRegistry_)
        .AddPreTypeAnnotation()
        .AddExpressionEvaluation(*FunctionRegistry_)
        .AddIOAnnotation()
        .AddTypeAnnotation(TIssuesIds::CORE_TYPE_ANN, true)
        .AddPostTypeAnnotation()
        .Add(TExprOutputTransformer::Sync(ExprRoot_, traceOut), "ExprOutput")
        .AddLineageOptimization(LineageStr_)
        .Add(TExprOutputTransformer::Sync(ExprRoot_, exprOut, withTypes), "AstOutput")
        .Build();

    TFuture<void> openSession = OpenSession(username);
    if (!openSession.Initialized())
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);

    SaveExprRoot();

    return openSession.Apply([this](const TFuture<void>& f) {
        YQL_LOG_CTX_ROOT_SESSION_SCOPE(GetSessionId());
        try {
            f.GetValue();
        } catch (const std::exception& e) {
            YQL_LOG(ERROR) << "OpenSession error: " << e.what();
            ExprCtx_->IssueManager.RaiseIssue(ExceptionToIssue(e));
            return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
        }
        return AsyncTransformWithFallback(false);
    });
}

TProgram::TStatus TProgram::Validate(const TString& username, IOutputStream* exprOut, bool withTypes) {
    YQL_PROFILE_FUNC(TRACE);
    auto m = &TProgram::ValidateAsync;
    return SyncExecution(this, m, username, exprOut, withTypes);
}

TProgram::TFutureStatus TProgram::ValidateAsync(const TString& username, IOutputStream* exprOut, bool withTypes) {
    if (!ProvideAnnotationContext(username)->Initialize(*ExprCtx_) || !CollectUsedClusters()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }
    TypeCtx_->IsReadOnly = true;

    TVector<TDataProviderInfo> dataProviders;
    with_lock (DataProvidersLock_) {
        dataProviders = DataProviders_;
    }

    for (const auto& dp : dataProviders) {
        if (!dp.RemoteClusterProvider || !dp.RemoteValidate) {
            continue;
        }

        if (auto cluster = dp.RemoteClusterProvider(UsedClusters_, UsedProviders_, SourceSyntax_)) {
            return dp.RemoteValidate(*cluster, SourceSyntax_, SourceCode_, *ExprCtx_);
        }
    }

    Y_ENSURE(ExprRoot_, "Program not compiled yet");

    ExprStream_ = exprOut;
    Transformer_ = TTransformationPipeline(TypeCtx_)
            .AddServiceTransformers()
            .AddParametersEvaluation(*FunctionRegistry_)
            .AddPreTypeAnnotation()
            .AddExpressionEvaluation(*FunctionRegistry_)
            .AddIOAnnotation()
            .AddTypeAnnotation(TIssuesIds::CORE_TYPE_ANN, true)
            .Add(TExprOutputTransformer::Sync(ExprRoot_, exprOut, withTypes), "AstOutput")
            .Build();

    TFuture<void> openSession = OpenSession(username);
    if (!openSession.Initialized()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }

    SaveExprRoot();

    return openSession.Apply([this](const TFuture<void>& f) {
        YQL_LOG_CTX_ROOT_SESSION_SCOPE(GetSessionId());
        try {
            f.GetValue();
        } catch (const std::exception& e) {
            YQL_LOG(ERROR) << "OpenSession error: " << e.what();
            ExprCtx_->IssueManager.RaiseIssue(ExceptionToIssue(e));
            return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
        }
        return AsyncTransformWithFallback(false);
    });
}

TProgram::TStatus TProgram::Optimize(
        const TString& username,
        IOutputStream* traceOut,
        IOutputStream* tracePlan,
        IOutputStream* exprOut,
        bool withTypes)
{
    YQL_PROFILE_FUNC(TRACE);
    auto m = &TProgram::OptimizeAsync;
    return SyncExecution(this, m, username, traceOut, tracePlan, exprOut, withTypes);
}

TProgram::TFutureStatus TProgram::OptimizeAsync(
        const TString& username,
        IOutputStream* traceOut,
        IOutputStream* tracePlan,
        IOutputStream* exprOut,
        bool withTypes)
{
    if (!ProvideAnnotationContext(username)->Initialize(*ExprCtx_) || !CollectUsedClusters()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }
    TypeCtx_->IsReadOnly = true;

    TVector<TDataProviderInfo> dataProviders;
    with_lock (DataProvidersLock_) {
        dataProviders = DataProviders_;
    }

    for (const auto& dp : dataProviders) {
        if (!dp.RemoteClusterProvider || !dp.RemoteOptimize) {
            continue;
        }

        if (auto cluster = dp.RemoteClusterProvider(UsedClusters_, UsedProviders_, SourceSyntax_)) {
            return dp.RemoteOptimize(*cluster,
                SourceSyntax_, SourceCode_, nullptr,
                TypeCtx_, ExprRoot_, *ExprCtx_, ExternalQueryAst_, ExternalQueryPlan_);
        }
    }

    Y_ENSURE(ExprRoot_, "Program not compiled yet");

    ExprStream_ = exprOut;
    PlanStream_ = tracePlan;
    Transformer_ = TTransformationPipeline(TypeCtx_)
        .AddServiceTransformers()
        .AddParametersEvaluation(*FunctionRegistry_)
        .AddPreTypeAnnotation()
        .AddExpressionEvaluation(*FunctionRegistry_)
        .AddIOAnnotation()
        .AddTypeAnnotation(TIssuesIds::CORE_TYPE_ANN, true)
        .AddPostTypeAnnotation()
        .Add(TExprOutputTransformer::Sync(ExprRoot_, traceOut), "ExprOutput")
        .AddOptimization()
        .Add(TExprOutputTransformer::Sync(ExprRoot_, exprOut, withTypes), "AstOutput")
        .Build();

    TFuture<void> openSession = OpenSession(username);
    if (!openSession.Initialized())
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);

    SaveExprRoot();

    return openSession.Apply([this](const TFuture<void>& f) {
        YQL_LOG_CTX_ROOT_SESSION_SCOPE(GetSessionId());
        try {
            f.GetValue();
        } catch (const std::exception& e) {
            YQL_LOG(ERROR) << "OpenSession error: " << e.what();
            ExprCtx_->IssueManager.RaiseIssue(ExceptionToIssue(e));
            return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
        }
        return AsyncTransformWithFallback(false);
    });
}

TProgram::TStatus TProgram::OptimizeWithConfig(
        const TString& username, const IPipelineConfigurator& pipelineConf)
{
    YQL_PROFILE_FUNC(TRACE);
    auto m = &TProgram::OptimizeAsyncWithConfig;
    return SyncExecution(this, m, username, pipelineConf);
}

TProgram::TFutureStatus TProgram::OptimizeAsyncWithConfig(
        const TString& username, const IPipelineConfigurator& pipelineConf)
{
    if (!ProvideAnnotationContext(username)->Initialize(*ExprCtx_) || !CollectUsedClusters()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }
    TypeCtx_->IsReadOnly = true;

    TVector<TDataProviderInfo> dataProviders;
    with_lock (DataProvidersLock_) {
        dataProviders = DataProviders_;
    }

    for (const auto& dp : dataProviders) {
        if (!dp.RemoteClusterProvider || !dp.RemoteOptimize) {
            continue;
        }

        if (auto cluster = dp.RemoteClusterProvider(UsedClusters_, UsedProviders_, SourceSyntax_)) {
            return dp.RemoteOptimize(*cluster,
                SourceSyntax_, SourceCode_, &pipelineConf,
                TypeCtx_, ExprRoot_, *ExprCtx_, ExternalQueryAst_, ExternalQueryPlan_);
        }
    }

    Y_ENSURE(ExprRoot_, "Program not compiled yet");

    TTransformationPipeline pipeline(TypeCtx_);
    pipelineConf.AfterCreate(&pipeline);
    pipeline.AddServiceTransformers();
    pipeline.AddParametersEvaluation(*FunctionRegistry_);
    pipeline.AddPreTypeAnnotation();
    pipeline.AddExpressionEvaluation(*FunctionRegistry_);
    pipeline.AddIOAnnotation();
    pipeline.AddTypeAnnotation(TIssuesIds::CORE_TYPE_ANN, true);
    pipeline.AddPostTypeAnnotation();
    pipelineConf.AfterTypeAnnotation(&pipeline);

    pipeline.AddOptimization();
    if (EnableRangeComputeFor_) {
        pipeline.Add(MakeExpandRangeComputeForTransformer(pipeline.GetTypeAnnotationContext()),
                     "ExpandRangeComputeFor", TIssuesIds::CORE_EXEC);
    }

    pipeline.Add(CreatePlanInfoTransformer(*TypeCtx_), "PlanInfo");
    pipelineConf.AfterOptimize(&pipeline);

    Transformer_ = pipeline.Build();

    TFuture<void> openSession = OpenSession(username);
    if (!openSession.Initialized())
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);

    SaveExprRoot();

    return openSession.Apply([this](const TFuture<void>& f) {
        YQL_LOG_CTX_ROOT_SESSION_SCOPE(GetSessionId());
        try {
            f.GetValue();
        } catch (const std::exception& e) {
            YQL_LOG(ERROR) << "OpenSession error: " << e.what();
            ExprCtx_->IssueManager.RaiseIssue(ExceptionToIssue(e));
            return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
        }
        return AsyncTransformWithFallback(false);
    });
}

TProgram::TStatus TProgram::LineageWithConfig(
        const TString& username, const IPipelineConfigurator& pipelineConf)
{
    YQL_PROFILE_FUNC(TRACE);
    auto m = &TProgram::LineageAsyncWithConfig;
    return SyncExecution(this, m, username, pipelineConf);
}

TProgram::TFutureStatus TProgram::LineageAsyncWithConfig(
        const TString& username, const IPipelineConfigurator& pipelineConf)
{
    if (!ProvideAnnotationContext(username)->Initialize(*ExprCtx_) || !CollectUsedClusters()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }
    TypeCtx_->IsReadOnly = true;

    Y_ENSURE(ExprRoot_, "Program not compiled yet");

    TTransformationPipeline pipeline(TypeCtx_);
    pipelineConf.AfterCreate(&pipeline);
    pipeline.AddServiceTransformers();
    pipeline.AddParametersEvaluation(*FunctionRegistry_);
    pipeline.AddPreTypeAnnotation();
    pipeline.AddExpressionEvaluation(*FunctionRegistry_);
    pipeline.AddIOAnnotation();
    pipeline.AddTypeAnnotation(TIssuesIds::CORE_TYPE_ANN, true);
    pipeline.AddPostTypeAnnotation();
    pipelineConf.AfterTypeAnnotation(&pipeline);

    pipeline.AddLineageOptimization(LineageStr_);

    Transformer_ = pipeline.Build();

    TFuture<void> openSession = OpenSession(username);
    if (!openSession.Initialized())
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);

    SaveExprRoot();

    return openSession.Apply([this](const TFuture<void>& f) {
        YQL_LOG_CTX_ROOT_SESSION_SCOPE(GetSessionId());
        try {
            f.GetValue();
        } catch (const std::exception& e) {
            YQL_LOG(ERROR) << "OpenSession error: " << e.what();
            ExprCtx_->IssueManager.RaiseIssue(ExceptionToIssue(e));
            return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
        }
        return AsyncTransformWithFallback(false);
    });
}

TProgram::TStatus TProgram::Run(
        const TString& username,
        IOutputStream* traceOut,
        IOutputStream* tracePlan,
        IOutputStream* exprOut,
        bool withTypes)
{
    YQL_PROFILE_FUNC(TRACE);
    auto m = &TProgram::RunAsync;
    return SyncExecution(this, m, username, traceOut, tracePlan, exprOut, withTypes);
}

TProgram::TFutureStatus TProgram::RunAsync(
        const TString& username,
        IOutputStream* traceOut,
        IOutputStream* tracePlan,
        IOutputStream* exprOut,
        bool withTypes)
{
    if (!ProvideAnnotationContext(username)->Initialize(*ExprCtx_) || !CollectUsedClusters()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }
    TypeCtx_->IsReadOnly = (HiddenMode_ != EHiddenMode::Disable);

    TVector<TDataProviderInfo> dataProviders;
    with_lock (DataProvidersLock_) {
        dataProviders = DataProviders_;
    }

    for (const auto& dp : dataProviders) {
        if (!dp.RemoteClusterProvider || !dp.RemoteRun) {
            continue;
        }

        if (auto cluster = dp.RemoteClusterProvider(UsedClusters_, UsedProviders_, SourceSyntax_)) {
            return dp.RemoteRun(*cluster, SourceSyntax_, SourceCode_,
                OutputFormat_, ResultFormat_, nullptr,
                TypeCtx_, ExprRoot_, *ExprCtx_, ExternalQueryAst_, ExternalQueryPlan_, ExternalDiagnostics_,
                ResultProviderConfig_);
        }
    }

    Y_ENSURE(ExprRoot_, "Program not compiled yet");

    ExprStream_ = exprOut;
    PlanStream_ = tracePlan;

    TTransformationPipeline pipeline(TypeCtx_);
    pipeline.AddServiceTransformers();
    pipeline.AddParametersEvaluation(*FunctionRegistry_);
    pipeline.AddPreTypeAnnotation();
    pipeline.AddExpressionEvaluation(*FunctionRegistry_);
    pipeline.AddIOAnnotation();
    pipeline.AddTypeAnnotation(TIssuesIds::CORE_TYPE_ANN, true);
    pipeline.AddPostTypeAnnotation();
    pipeline.Add(TExprOutputTransformer::Sync(ExprRoot_, traceOut), "ExprOutput");
    pipeline.AddOptimization();
    if (EnableRangeComputeFor_) {
        pipeline.Add(MakeExpandRangeComputeForTransformer(pipeline.GetTypeAnnotationContext()),
                     "ExpandRangeComputeFor", TIssuesIds::CORE_EXEC);
    }
    pipeline.Add(TExprOutputTransformer::Sync(ExprRoot_, exprOut, withTypes), "AstOutput");
    pipeline.Add(TPlanOutputTransformer::Sync(tracePlan, GetPlanBuilder(), OutputFormat_), "PlanOutput");
    pipeline.AddRun(ProgressWriter_);

    Transformer_ = pipeline.Build();

    TFuture<void> openSession = OpenSession(username);
    if (!openSession.Initialized()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }

    SaveExprRoot();

    return openSession.Apply([this](const TFuture<void>& f) {
        YQL_LOG_CTX_ROOT_SESSION_SCOPE(GetSessionId());
        try {
            f.GetValue();
        } catch (const std::exception& e) {
            YQL_LOG(ERROR) << "OpenSession error: " << e.what();
            ExprCtx_->IssueManager.RaiseIssue(ExceptionToIssue(e));
            return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
        }
        return AsyncTransformWithFallback(false);
    });
}

TProgram::TStatus TProgram::RunWithConfig(
        const TString& username, const IPipelineConfigurator& pipelineConf)
{
    YQL_PROFILE_FUNC(TRACE);
    auto m = &TProgram::RunAsyncWithConfig;
    return SyncExecution(this, m, username, pipelineConf);
}

TProgram::TFutureStatus TProgram::RunAsyncWithConfig(
        const TString& username, const IPipelineConfigurator& pipelineConf)
{
    if (!ProvideAnnotationContext(username)->Initialize(*ExprCtx_) || !CollectUsedClusters()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }
    TypeCtx_->IsReadOnly = (HiddenMode_ != EHiddenMode::Disable);

    TVector<TDataProviderInfo> dataProviders;
    with_lock (DataProvidersLock_) {
        dataProviders = DataProviders_;
    }

    for (const auto& dp : dataProviders) {
        if (!dp.RemoteClusterProvider || !dp.RemoteRun) {
            continue;
        }

        if (auto cluster = dp.RemoteClusterProvider(UsedClusters_, UsedProviders_, SourceSyntax_)) {
            return dp.RemoteRun(*cluster, SourceSyntax_, SourceCode_,
                OutputFormat_, ResultFormat_, &pipelineConf,
                TypeCtx_, ExprRoot_, *ExprCtx_, ExternalQueryAst_, ExternalQueryPlan_, ExternalDiagnostics_,
                ResultProviderConfig_);
        }
    }

    Y_ENSURE(ExprRoot_, "Program not compiled yet");

    TTransformationPipeline pipeline(TypeCtx_);
    pipelineConf.AfterCreate(&pipeline);
    pipeline.AddServiceTransformers();
    pipeline.AddParametersEvaluation(*FunctionRegistry_);
    pipeline.AddPreTypeAnnotation();
    pipeline.AddExpressionEvaluation(*FunctionRegistry_);
    pipeline.AddIOAnnotation();
    pipeline.AddTypeAnnotation(TIssuesIds::CORE_TYPE_ANN, true);
    pipeline.AddPostTypeAnnotation();
    pipelineConf.AfterTypeAnnotation(&pipeline);

    pipeline.AddOptimization();
    if (EnableRangeComputeFor_) {
        pipeline.Add(MakeExpandRangeComputeForTransformer(pipeline.GetTypeAnnotationContext()),
            "ExpandRangeComputeFor", TIssuesIds::CORE_EXEC);
    }
    pipelineConf.AfterOptimize(&pipeline);
    pipeline.AddRun(ProgressWriter_);

    Transformer_ = pipeline.Build();

    TFuture<void> openSession = OpenSession(username);
    if (!openSession.Initialized()) {
        return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
    }

    SaveExprRoot();

    return openSession.Apply([this](const TFuture<void>& f) {
        YQL_LOG_CTX_ROOT_SESSION_SCOPE(GetSessionId());
        try {
            f.GetValue();
        } catch (const std::exception& e) {
            YQL_LOG(ERROR) << "OpenSession error: " << e.what();
            ExprCtx_->IssueManager.RaiseIssue(ExceptionToIssue(e));
            return NThreading::MakeFuture<TStatus>(IGraphTransformer::TStatus::Error);
        }
        return AsyncTransformWithFallback(false);
    });
}

void TProgram::SaveExprRoot() {
    TNodeOnNodeOwnedMap deepClones;
    SavedExprRoot_ = ExprCtx_->DeepCopy(*ExprRoot_, *ExprCtx_, deepClones, /*internStrings*/false, /*copyTypes*/true, /*copyResult*/false, {});
}

std::optional<bool> TProgram::CheckFallbackIssues(const TIssues& issues) {
    auto isFallback = std::optional<bool>();
    auto checkIssue = [&](const TIssue& issue) {
        if (issue.GetCode() == TIssuesIds::DQ_GATEWAY_ERROR) {
            YQL_LOG(DEBUG) << "Gateway Error " << issue;
            isFallback = false;
        } else if (issue.GetCode() == TIssuesIds::DQ_GATEWAY_NEED_FALLBACK_ERROR) {
            YQL_LOG(DEBUG) << "Gateway Fallback Error " << issue;
            isFallback = true;
        } else if (issue.GetCode() == TIssuesIds::DQ_OPTIMIZE_ERROR) {
            YQL_LOG(DEBUG) << "Optimize Error " << issue;
            isFallback = true;
        } else if (issue.GetCode() >= TIssuesIds::YT_ACCESS_DENIED &&
                   issue.GetCode() <= TIssuesIds::YT_FOLDER_INPUT_IS_NOT_A_FOLDER &&
                  (issue.GetSeverity() == TSeverityIds::S_ERROR ||
                   issue.GetSeverity() == TSeverityIds::S_FATAL)) {
            YQL_LOG(DEBUG) << "Yt Error " << issue;
            isFallback = false;
        }
    };

    std::function<void(const TIssuePtr& issue)> recursiveCheck = [&](const TIssuePtr& issue) {
        checkIssue(*issue);
        for (const auto& subissue : issue->GetSubIssues()) {
            recursiveCheck(subissue);
        }
    };

    for (const auto& issue : issues) {
        checkIssue(issue);
        // check subissues
        for (const auto& subissue : issue.GetSubIssues()) {
            recursiveCheck(subissue);
        }
    }
    return isFallback;
}

TFuture<IGraphTransformer::TStatus> TProgram::AsyncTransformWithFallback(bool applyAsyncChanges)
{
    return AsyncTransform(*Transformer_, ExprRoot_, *ExprCtx_, applyAsyncChanges).Apply([this](const TFuture<IGraphTransformer::TStatus>& res) {
        auto status = res.GetValueSync();
        if (status == IGraphTransformer::TStatus::Error
            && !TypeCtx_->ForceDq
            && SavedExprRoot_
            && TypeCtx_->DqCaptured
            && TypeCtx_->DqFallbackPolicy != EFallbackPolicy::Never)
        {
            auto issues = ExprCtx_->IssueManager.GetIssues();
            bool isFallback = CheckFallbackIssues(issues).value_or(true);

            if (!isFallback && TypeCtx_->DqFallbackPolicy != EFallbackPolicy::Always) {
                // unrecoverable error
                return res;
            }

            ExprRoot_ = SavedExprRoot_;
            SavedExprRoot_ = nullptr;
            UserDataStorage_->SetUserDataTable(std::move(SavedUserDataTable_));

            ExprCtx_->IssueManager.Reset();
            YQL_LOG(DEBUG) << "Fallback, Issues: " << issues.ToString();

            ExprCtx_->Reset();
            Transformer_->Rewind();

            for (auto sink : TypeCtx_->DataSinks) {
                sink->Reset();
            }
            for (auto source : TypeCtx_->DataSources) {
                source->Reset();
            }
            TypeCtx_->Reset();
            try {
                CleanupLastSession().GetValueSync();
            } catch (...) {
                ExprCtx_->IssueManager.RaiseIssue(TIssue({}, "Failed to cleanup session: " + CurrentExceptionMessage()));
                return NThreading::MakeFuture<IGraphTransformer::TStatus>(IGraphTransformer::TStatus::Error);
            }

            std::function<void(const TIssuePtr& issue)> toInfo = [&](const TIssuePtr& issue) {
                if (issue->Severity == TSeverityIds::S_ERROR
                    || issue->Severity == TSeverityIds::S_FATAL
                    || issue->Severity == TSeverityIds::S_WARNING)
                {
                    issue->Severity = TSeverityIds::S_INFO;
                }
                for (const auto& subissue : issue->GetSubIssues()) {
                    toInfo(subissue);
                }
            };

            TIssue info("DQ cannot execute the query");
            info.Severity = TSeverityIds::S_INFO;

            for (auto& issue : issues) {
                TIssuePtr newIssue = new TIssue(issue);
                if (newIssue->Severity == TSeverityIds::S_ERROR
                    || issue.Severity == TSeverityIds::S_FATAL
                    || issue.Severity == TSeverityIds::S_WARNING)
                {
                    newIssue->Severity = TSeverityIds::S_INFO;
                }
                for (auto& subissue : newIssue->GetSubIssues()) {
                    toInfo(subissue);
                }
                info.AddSubIssue(newIssue);
            }

            ExprCtx_->IssueManager.AddIssues({info});

            ++FallbackCounter_;
            // don't execute recapture again
            ExprCtx_->Step.Done(TExprStep::Recapture);
            AbortHidden_();
            return AsyncTransformWithFallback(false);
        }
        if (status == IGraphTransformer::TStatus::Error && (TypeCtx_->DqFallbackPolicy == EFallbackPolicy::Never || TypeCtx_->ForceDq)) {
            YQL_LOG(INFO) << "Fallback skipped due to per query policy";
        }
        return res;
    });
}

TMaybe<TString> TProgram::GetQueryAst(TMaybe<size_t> memoryLimit) {
    if (ExternalQueryAst_) {
        return ExternalQueryAst_;
    }

    TStringStream astStream;
    astStream.Reserve(DEFAULT_AST_BUF_SIZE);

    if (ExprRoot_) {
        std::unique_ptr<IAllocator> limitingAllocator;
        TConvertToAstSettings settings;
        settings.AnnotationFlags = TExprAnnotationFlags::None;
        settings.RefAtoms = true;
        settings.Allocator = TDefaultAllocator::Instance();
        if (memoryLimit) {
            limitingAllocator = MakeLimitingAllocator(*memoryLimit, TDefaultAllocator::Instance());
            settings.Allocator = limitingAllocator.get();
        }

        auto ast = ConvertToAst(*ExprRoot_, *ExprCtx_, settings);
        ast.Root->PrettyPrintTo(astStream, TAstPrintFlags::ShortQuote | TAstPrintFlags::PerLine);
        return astStream.Str();
    } else if (AstRoot_) {
        AstRoot_->PrettyPrintTo(astStream, TAstPrintFlags::ShortQuote | TAstPrintFlags::PerLine);
        return astStream.Str();
    }

    return Nothing();
}

TMaybe<TString> TProgram::GetQueryPlan(const TPlanSettings& settings) {
    if (ExternalQueryPlan_) {
        return ExternalQueryPlan_;
    }

    if (ExprRoot_) {
        TStringStream planStream;
        planStream.Reserve(DEFAULT_PLAN_BUF_SIZE);

        NYson::TYsonWriter writer(&planStream, OutputFormat_);
        PlanBuilder_->WritePlan(writer, ExprRoot_, settings);

        return planStream.Str();
    }

    return Nothing();
}

TMaybe<TString> TProgram::GetDiagnostics() {
    if (ExternalDiagnostics_) {
        return ExternalDiagnostics_;
    }

    if (!TypeCtx_ || !TypeCtx_->Diagnostics) {
        return Nothing();
    }

    if (!Transformer_) {
        return Nothing();
    }

    TStringStream out;
    NYson::TYsonWriter writer(&out, DiagnosticFormat_.GetOrElse(ResultFormat_));

    writer.OnBeginMap();
    writer.OnKeyedItem("Write");
    writer.OnBeginList();
    writer.OnListItem();
    writer.OnBeginMap();
    writer.OnKeyedItem("Data");
    writer.OnBeginMap();
    writer.OnKeyedItem("Diagnostics");
    writer.OnBeginMap();
    writer.OnKeyedItem("TransformStats");

    auto transformStats = Transformer_->GetStatistics();
    NCommon::TransformerStatsToYson("", transformStats, writer);

    for (auto& datasink : TypeCtx_->DataSinks) {
        writer.OnKeyedItem(datasink->GetName());
        if (!datasink->CollectDiagnostics(writer)) {
            writer.OnEntity();
        }
    }

    writer.OnEndMap();
    writer.OnEndMap();
    writer.OnEndMap();
    writer.OnEndList();
    writer.OnEndMap();

    return out.Str();
}

IGraphTransformer::TStatistics TProgram::GetRawDiagnostics() {
    return Transformer_ ? Transformer_->GetStatistics() : IGraphTransformer::TStatistics::NotPresent();
}

TMaybe<TString> TProgram::GetTasksInfo() {
    if (!TypeCtx_) {
        return Nothing();
    }

    bool hasTasks = false;

    TStringStream out;
    NYson::TYsonWriter writer(&out, ResultFormat_);

    writer.OnBeginMap();
    writer.OnKeyedItem("Write");
    writer.OnBeginList();
    writer.OnListItem();
    writer.OnBeginMap();
    writer.OnKeyedItem("Tasks");
    writer.OnBeginList();

    for (auto& datasink : TypeCtx_->DataSinks) {
        hasTasks = hasTasks || datasink->GetTasksInfo(writer);
    }

    writer.OnEndList();
    writer.OnEndMap();
    writer.OnEndList();
    writer.OnEndMap();

    if (hasTasks) {
        return out.Str();
    } else {
        return Nothing();
    }
}

TMaybe<TString> TProgram::GetStatistics(bool totalOnly, THashMap<TString, TStringBuf> extraYsons) {
    if (!TypeCtx_) {
        return Nothing();
    }

    TStringStream out;
    NYson::TYsonWriter writer(&out, OutputFormat_);
    // Header
    writer.OnBeginMap();
    writer.OnKeyedItem("ExecutionStatistics");
    writer.OnBeginMap();

    // Providers
    THashSet<TStringBuf> processed;
    for (auto& datasink : TypeCtx_->DataSinks) {
        TStringStream providerOut;
        NYson::TYsonWriter providerWriter(&providerOut);
        if (datasink->CollectStatistics(providerWriter, totalOnly)) {
            writer.OnKeyedItem(datasink->GetName());
            writer.OnRaw(providerOut.Str());
            processed.insert(datasink->GetName());
        }
    }
    for (auto& datasource : TypeCtx_->DataSources) {
        if (processed.insert(datasource->GetName()).second) {
            TStringStream providerOut;
            NYson::TYsonWriter providerWriter(&providerOut);
            if (datasource->CollectStatistics(providerWriter, totalOnly)) {
                writer.OnKeyedItem(datasource->GetName());
                writer.OnRaw(providerOut.Str());
            }
        }
    }

    auto rusage = TRusage::Get();
    // System stats
    writer.OnKeyedItem("system");
    writer.OnBeginMap();
        writer.OnKeyedItem("MaxRSS");
        writer.OnBeginMap();
            writer.OnKeyedItem("max");
            writer.OnInt64Scalar(rusage.MaxRss);
        writer.OnEndMap();

        writer.OnKeyedItem("MajorPageFaults");
        writer.OnBeginMap();
            writer.OnKeyedItem("count");
            writer.OnInt64Scalar(rusage.MajorPageFaults);
        writer.OnEndMap();

        if (FallbackCounter_) {
            writer.OnKeyedItem("Fallback");
            writer.OnBeginMap();
                writer.OnKeyedItem("count");
                writer.OnInt64Scalar(FallbackCounter_);
            writer.OnEndMap();
        }

    writer.OnEndMap(); // system

    if (TypeCtx_->Modules) {
        writer.OnKeyedItem("moduleResolver");
        writer.OnBeginMap();
        TypeCtx_->Modules->WriteStatistics(writer);
        writer.OnEndMap();
    }

    // extra
    for (const auto &[k, extraYson] : extraYsons) {
        writer.OnKeyedItem(k);
        writer.OnRaw(extraYson);
    }

    // Footer
    writer.OnEndMap();
    writer.OnEndMap();
    return out.Str();
}

TMaybe<TString> TProgram::GetDiscoveredData() {
    if (!TypeCtx_) {
        return Nothing();
    }

    TStringStream out;
    NYson::TYsonWriter writer(&out, OutputFormat_);
    writer.OnBeginMap();
    for (auto& datasource: TypeCtx_->DataSources) {
        TStringStream providerOut;
        NYson::TYsonWriter providerWriter(&providerOut);
        if (datasource->CollectDiscoveredData(providerWriter)) {
            writer.OnKeyedItem(datasource->GetName());
            writer.OnRaw(providerOut.Str());
        }
    }
    writer.OnEndMap();
    return out.Str();
}

TMaybe<TString> TProgram::GetLineage() {
    return LineageStr_;
}

TProgram::TFutureStatus TProgram::ContinueAsync() {
    YQL_LOG_CTX_ROOT_SESSION_SCOPE(GetSessionId());
    return AsyncTransformWithFallback(true);
}

NThreading::TFuture<void> TProgram::Abort()
{
    return CloseLastSession();
}

TIssues TProgram::Issues() const {
    TIssues result;
    if (ExprCtx_) {
        result.AddIssues(ExprCtx_->IssueManager.GetIssues());
    }
    result.AddIssues(FinalIssues_);
    CheckFatalIssues(result);
    return result;
}

TIssues TProgram::CompletedIssues() const {
    TIssues result;
    if (ExprCtx_) {
        result.AddIssues(ExprCtx_->IssueManager.GetCompletedIssues());
    }
    result.AddIssues(FinalIssues_);
    CheckFatalIssues(result);
    return result;
}

TIssue MakeNoBlocksInfoIssue(const TVector<TString>& names, bool isTypes) {
    TIssue result;
    TString msg = TStringBuilder() << "Most frequent " << (isTypes ? "types " : "callables ")
                                   << "which do not support block mode: " << JoinRange(", ", names.begin(), names.end());
    result.SetMessage(msg);
    result.SetCode(isTypes ? TIssuesIds::CORE_TOP_UNSUPPORTED_BLOCK_TYPES : TIssuesIds::CORE_TOP_UNSUPPORTED_BLOCK_CALLABLES, TSeverityIds::S_INFO);
    return result;
}

void TProgram::FinalizeIssues() {
    FinalIssues_.Clear();
    if (TypeCtx_) {
        static const size_t topCount = 10;
        auto noBlockTypes = TypeCtx_->GetTopNoBlocksTypes(topCount);
        if (!noBlockTypes.empty()) {
            FinalIssues_.AddIssue(MakeNoBlocksInfoIssue(noBlockTypes, true));
        }
        auto noBlockCallables = TypeCtx_->GetTopNoBlocksCallables(topCount);
        if (!noBlockCallables.empty()) {
            FinalIssues_.AddIssue(MakeNoBlocksInfoIssue(noBlockCallables, false));
        }
    }
}

NThreading::TFuture<void> TProgram::CleanupLastSession() {
    YQL_LOG_CTX_ROOT_SESSION_SCOPE(GetSessionId());

    TString sessionId = GetSessionId();
    if (sessionId.empty()) {
        return MakeFuture();
    }

    TVector<TDataProviderInfo> dataProviders;
    with_lock (DataProvidersLock_) {
        dataProviders = DataProviders_;
    }

    TVector<NThreading::TFuture<void>> cleanupFutures;
    cleanupFutures.reserve(dataProviders.size());
    for (const auto& dp : dataProviders) {
        if (dp.CleanupSession) {
            dp.CleanupSession(sessionId);
        }
        if (dp.CleanupSessionAsync) {
            cleanupFutures.push_back(dp.CleanupSessionAsync(sessionId));
        }
    }

    return NThreading::WaitExceptionOrAll(cleanupFutures);
}

NThreading::TFuture<void> TProgram::CloseLastSession() {
    YQL_LOG_CTX_ROOT_SESSION_SCOPE(GetSessionId());

    TVector<TDataProviderInfo> dataProviders;
    with_lock (DataProvidersLock_) {
        dataProviders = DataProviders_;
    }

    auto promise = NThreading::NewPromise<void>();

    TString sessionId;
    with_lock(SessionIdLock_) {
        // post-condition: SessionId_ will be empty
        sessionId = std::move(SessionId_);
        if (sessionId.empty()) {
            return CloseLastSessionFuture_;
        }

        CloseLastSessionFuture_ = promise.GetFuture();
    }

    TVector<NThreading::TFuture<void>> closeFutures;
    closeFutures.reserve(dataProviders.size());
    for (const auto& dp : dataProviders) {
        if (dp.CloseSession) {
            dp.CloseSession(sessionId);
        }
        if (dp.CloseSessionAsync) {
            closeFutures.push_back(dp.CloseSessionAsync(sessionId));
        }
    }

    return NThreading::WaitExceptionOrAll(closeFutures)
        .Apply([promise = std::move(promise)](const NThreading::TFuture<void>&) mutable {
            promise.SetValue();
        });
}

TString TProgram::ResultsAsString() const {
    if (!ResultProviderConfig_)
        return "";

    TStringStream resultOut;
    NYson::TYsonWriter yson(&resultOut, OutputFormat_);
    yson.OnBeginList();
    for (const auto& result: Results()) {
        yson.OnListItem();
        yson.OnRaw(result);
    }
    yson.OnEndList();
    return resultOut.Str();
}

TTypeAnnotationContextPtr TProgram::BuildTypeAnnotationContext(const TString& username) {
    auto typeAnnotationContext = MakeIntrusive<TTypeAnnotationContext>();

    typeAnnotationContext->LangVer = LangVer_;
    typeAnnotationContext->UseTypeDiffForConvertToError = true;
    typeAnnotationContext->UserDataStorage = UserDataStorage_;
    typeAnnotationContext->Credentials = Credentials_;
    typeAnnotationContext->Modules = Modules_;
    typeAnnotationContext->UrlListerManager = UrlListerManager_;
    typeAnnotationContext->UdfResolver = UdfResolver_;
    typeAnnotationContext->UdfIndex = UdfIndex_;
    typeAnnotationContext->UdfIndexPackageSet = UdfIndexPackageSet_;
    typeAnnotationContext->RandomProvider = RandomProvider_;
    typeAnnotationContext->TimeProvider = TimeProvider_;
    if (DiagnosticFormat_) {
        typeAnnotationContext->Diagnostics = true;
    }
    typeAnnotationContext->ArrowResolver = ArrowResolver_;
    typeAnnotationContext->FileStorage = FileStorage_;
    typeAnnotationContext->QContext = QContext_;
    typeAnnotationContext->HiddenMode = HiddenMode_;

    if (UdfIndex_ && UdfIndexPackageSet_) {
        // setup default versions at the beginning
        // could be overridden by pragma later
        UdfIndexPackageSet_->AddResourcesTo(UdfIndex_);
    }

    PlanBuilder_ = CreatePlanBuilder(*typeAnnotationContext);
    THashSet<TString> providerNames;
    TVector<TString> fullResultDataSinks;
    TVector<std::function<TString(const TString&, const TString&)>> tokenResolvers;
    for (const auto& dpi : DataProvidersInit_) {
        auto dp = dpi(
            username,
            SessionId_,
            GatewaysConfig_,
            FunctionRegistry_,
            RandomProvider_,
            typeAnnotationContext,
            ProgressWriter_,
            OperationOptions_,
            AbortHidden_,
            QContext_
        );
        if (HiddenMode_ != EHiddenMode::Disable && !dp.SupportsHidden) {
            continue;
        }

        providerNames.insert(dp.Names.begin(), dp.Names.end());
        with_lock (DataProvidersLock_) {
            DataProviders_.emplace_back(dp);
        }
        if (dp.Source) {
            typeAnnotationContext->AddDataSource(dp.Names, dp.Source);
        }

        if (dp.Sink) {
            typeAnnotationContext->AddDataSink(dp.Names, dp.Sink);
        }

        if (dp.TokenResolver) {
            tokenResolvers.push_back(dp.TokenResolver);
        }

        if (dp.SupportFullResultDataSink) {
            fullResultDataSinks.insert(fullResultDataSinks.end(), dp.Names.begin(), dp.Names.end());
        }
    }

    TVector<TString> resultProviderDataSources;
    if (providerNames.contains(YtProviderName)) {
        resultProviderDataSources.push_back(TString(YtProviderName));
    }

    if (providerNames.contains(KikimrProviderName)) {
        resultProviderDataSources.push_back(TString(KikimrProviderName));
    }

    if (providerNames.contains(RtmrProviderName)) {
        resultProviderDataSources.push_back(TString(RtmrProviderName));
    }

    if (providerNames.contains(PqProviderName)) {
        resultProviderDataSources.push_back(TString(PqProviderName));
    }

    if (providerNames.contains(DqProviderName)) {
        resultProviderDataSources.push_back(TString(DqProviderName));
    }

    if (providerNames.contains(PureProviderName)) {
        resultProviderDataSources.push_back(TString(PureProviderName));
    }

    if (!resultProviderDataSources.empty())
    {
        auto resultFormat = ResultFormat_;
        auto writerFactory = [resultFormat] () { return CreateYsonResultWriter(resultFormat); };
        ResultProviderConfig_ = MakeIntrusive<TResultProviderConfig>(*typeAnnotationContext,
            *FunctionRegistry_, ResultType_, ToString((ui32)resultFormat), writerFactory);
        ResultProviderConfig_->SupportsResultPosition = SupportsResultPosition_;
        if (VolatileResults_) {
            constexpr ui64 BytesLimitForVolatileResults = 10*1024*1024;
            ResultProviderConfig_->FillSettings.AllResultsBytesLimit = BytesLimitForVolatileResults;
            ResultProviderConfig_->FillSettings.RowsLimitPerWrite.Clear();
        }

        auto resultProvider = CreateResultProvider(ResultProviderConfig_);
        typeAnnotationContext->AddDataSink(ResultProviderName, resultProvider);
        typeAnnotationContext->AvailablePureResultDataSources = resultProviderDataSources;
    }

    if (!fullResultDataSinks.empty()) {
        typeAnnotationContext->FullResultDataSink = fullResultDataSinks.front();
    }

    {
        auto configProvider = CreateConfigProvider(*typeAnnotationContext, GatewaysConfig_, username);
        typeAnnotationContext->AddDataSource(ConfigProviderName, configProvider);
    }

    tokenResolvers.push_back(BuildDefaultTokenResolver(typeAnnotationContext->Credentials));
    typeAnnotationContext->UserDataStorage->SetTokenResolver(BuildCompositeTokenResolver(std::move(tokenResolvers)));

    return typeAnnotationContext;
}

TFuture<void> TProgram::OpenSession(const TString& username)
{
    TVector<TFuture<void>> openFutures;
    with_lock (DataProvidersLock_) {
        for (const auto& dp : DataProviders_) {
            if (dp.OpenSession) {
                auto future = dp.OpenSession(SessionId_, username, ProgressWriter_, OperationOptions_,
                    RandomProvider_, TimeProvider_);
                openFutures.push_back(future);
            }
        }
    }

    return WaitExceptionOrAll(openFutures);
}

void TProgram::Print(IOutputStream* exprOut, IOutputStream* planOut, bool cleanPlan) {
    TVector<TTransformStage> printTransformers;
    const auto issueCode = TIssuesIds::DEFAULT_ERROR;
    if (exprOut) {
        printTransformers.push_back(TTransformStage(
            TExprOutputTransformer::Sync(ExprRoot_, exprOut),
            "ExprOutput",
            issueCode));
    }
    if (planOut) {
        if (cleanPlan) {
            GetPlanBuilder().Clear();
        }
        printTransformers.push_back(TTransformStage(
            TPlanOutputTransformer::Sync(planOut, GetPlanBuilder(), OutputFormat_),
            "PlanOutput",
            issueCode));
    }

    auto compositeTransformer = CreateCompositeGraphTransformer(printTransformers, false);
    InstantTransform(*compositeTransformer, ExprRoot_, *ExprCtx_);
}

bool TProgram::HasActiveProcesses() {
    with_lock (DataProvidersLock_) {
        for (const auto& dp : DataProviders_) {
            if (dp.HasActiveProcesses && dp.HasActiveProcesses()) {
                return true;
            }
        }
    }

    return false;
}

bool TProgram::NeedWaitForActiveProcesses() {
    with_lock (DataProvidersLock_) {
        for (const auto& dp : DataProviders_) {
            if (dp.HasActiveProcesses && dp.HasActiveProcesses() && dp.WaitForActiveProcesses) {
                return true;
            }
        }
    }

    return false;
}

} // namespace NYql
