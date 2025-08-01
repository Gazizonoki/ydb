#include "kqp_translate.h"

#include <ydb/core/kqp/provider/yql_kikimr_results.h>
#include <yql/essentials/sql/sql.h>
#include <yql/essentials/sql/v0/sql.h>
#include <yql/essentials/sql/v1/sql.h>
#include <yql/essentials/sql/v1/lexer/antlr3/lexer.h>
#include <yql/essentials/sql/v1/lexer/antlr3_ansi/lexer.h>
#include <yql/essentials/sql/v1/proto_parser/antlr3/proto_parser.h>
#include <yql/essentials/sql/v1/proto_parser/antlr3_ansi/proto_parser.h>
#include <yql/essentials/sql/v1/lexer/antlr4/lexer.h>
#include <yql/essentials/sql/v1/lexer/antlr4_ansi/lexer.h>
#include <yql/essentials/sql/v1/proto_parser/antlr4/proto_parser.h>
#include <yql/essentials/sql/v1/proto_parser/antlr4_ansi/proto_parser.h>

#include <yql/essentials/parser/pg_wrapper/interface/parser.h>
#include <ydb/public/api/protos/ydb_query.pb.h>


namespace NKikimr {
namespace NKqp {

TKqpAutoParamBuilder::TKqpAutoParamBuilder()
    : TypeProxy(*this)
    , DataProxy(*this)
{}

ui32 TKqpAutoParamBuilder::Size() const {
    return Values.size();
}

bool TKqpAutoParamBuilder::Contains(const TString& name) const {
    return Values.contains(name);
}

NYql::IAutoParamTypeBuilder& TKqpAutoParamBuilder::Add(const TString& name) {
    auto [it, inserted] = Values.emplace(name, Ydb::TypedValue{});
    CurrentParam = &it->second;
    TypeProxy.CurrentType = CurrentParam->mutable_type();
    DataProxy.CurrentValue = CurrentParam->mutable_value();
    return TypeProxy;
}

TKqpAutoParamBuilder::TTypeProxy::TTypeProxy(TKqpAutoParamBuilder& owner)
    : Owner(owner)
{}

void TKqpAutoParamBuilder::TTypeProxy::Push() {
    Stack.push_back(CurrentType);
}

void TKqpAutoParamBuilder::TTypeProxy::Pop() {
    CurrentType = Stack.back();
    Stack.pop_back();
}

void TKqpAutoParamBuilder::TTypeProxy::Pg(const TString& name) {
    auto oid = NYql::NPg::LookupType(name).TypeId;
    CurrentType->mutable_pg_type()->set_oid(oid);
}

void TKqpAutoParamBuilder::TTypeProxy::BeginList() {
    Push();
    CurrentType = CurrentType->mutable_list_type()->mutable_item();
}

void TKqpAutoParamBuilder::TTypeProxy::EndList() {
    Pop();
}

void TKqpAutoParamBuilder::TTypeProxy::BeginTuple() {
}

void TKqpAutoParamBuilder::TTypeProxy::EndTuple() {
}

void TKqpAutoParamBuilder::TTypeProxy::BeforeItem() {
    Push();
    CurrentType = CurrentType->mutable_tuple_type()->add_elements();
}

void TKqpAutoParamBuilder::TTypeProxy::AfterItem() {
    Pop();
}

NYql::IAutoParamDataBuilder& TKqpAutoParamBuilder::TTypeProxy::FinishType() {
    CurrentType = nullptr;
    return Owner.DataProxy;
}

TKqpAutoParamBuilder::TDataProxy::TDataProxy(TKqpAutoParamBuilder& owner)
    : Owner(owner)
{}

void TKqpAutoParamBuilder::TDataProxy::Pg(const TMaybe<TString>& value) {
    if (!value) {
        CurrentValue->set_null_flag_value(NProtoBuf::NULL_VALUE);
    } else {
        CurrentValue->set_text_value(*value);
    }
}

void TKqpAutoParamBuilder::TDataProxy::Push() {
    Stack.push_back(CurrentValue);
}

void TKqpAutoParamBuilder::TDataProxy::Pop() {
    CurrentValue = Stack.back();
    Stack.pop_back();
}

void TKqpAutoParamBuilder::TDataProxy::BeginList() {
}

void TKqpAutoParamBuilder::TDataProxy::EndList() {
}

void TKqpAutoParamBuilder::TDataProxy::BeginTuple() {
}

void TKqpAutoParamBuilder::TDataProxy::EndTuple() {
}

void TKqpAutoParamBuilder::TDataProxy::BeforeItem() {
    Push();
    CurrentValue = CurrentValue->mutable_items()->Add();
}

void TKqpAutoParamBuilder::TDataProxy::AfterItem() {
    Pop();
}

NYql::IAutoParamBuilder& TKqpAutoParamBuilder::TDataProxy::FinishData() {
    Owner.CurrentParam = nullptr;
    CurrentValue = nullptr;
    return Owner;
}

NYql::IAutoParamBuilderPtr TKqpAutoParamBuilderFactory::MakeBuilder() {
    return MakeIntrusive<TKqpAutoParamBuilder>();
}

NSQLTranslation::EBindingsMode RemapBindingsMode(NKikimrConfig::TTableServiceConfig::EBindingsMode mode) {
    switch (mode) {
        case NKikimrConfig::TTableServiceConfig::BM_ENABLED:
            return NSQLTranslation::EBindingsMode::ENABLED;
        case NKikimrConfig::TTableServiceConfig::BM_DISABLED:
            return NSQLTranslation::EBindingsMode::DISABLED;
        case NKikimrConfig::TTableServiceConfig::BM_DROP_WITH_WARNING:
            return NSQLTranslation::EBindingsMode::DROP_WITH_WARNING;
        case NKikimrConfig::TTableServiceConfig::BM_DROP:
            return NSQLTranslation::EBindingsMode::DROP;
        default:
            return NSQLTranslation::EBindingsMode::ENABLED;
    }
}

NYql::EKikimrQueryType ConvertType(NKikimrKqp::EQueryType type) {
    switch (type) {
        case NKikimrKqp::QUERY_TYPE_SQL_SCRIPT:
            return NYql::EKikimrQueryType::YqlScript;

        case NKikimrKqp::QUERY_TYPE_SQL_SCRIPT_STREAMING:
            return NYql::EKikimrQueryType::YqlScriptStreaming;

        case NKikimrKqp::QUERY_TYPE_SQL_DML:
        case NKikimrKqp::QUERY_TYPE_AST_DML:
            return NYql::EKikimrQueryType::Dml;

        case NKikimrKqp::QUERY_TYPE_SQL_DDL:
            return NYql::EKikimrQueryType::Ddl;

        case NKikimrKqp::QUERY_TYPE_AST_SCAN:
        case NKikimrKqp::QUERY_TYPE_SQL_SCAN:
            return NYql::EKikimrQueryType::Scan;

        case NKikimrKqp::QUERY_TYPE_SQL_GENERIC_QUERY:
        case NKikimrKqp::QUERY_TYPE_SQL_GENERIC_CONCURRENT_QUERY:
            return NYql::EKikimrQueryType::Query;

        case NKikimrKqp::QUERY_TYPE_SQL_GENERIC_SCRIPT:
            return NYql::EKikimrQueryType::Script;

        case NKikimrKqp::QUERY_TYPE_PREPARED_DML:
        case NKikimrKqp::QUERY_TYPE_UNDEFINED:
            YQL_ENSURE(false, "Unexpected query type: " << type);
    }
}

TKqpTranslationSettingsBuilder& TKqpTranslationSettingsBuilder::SetFromConfig(const NYql::TKikimrConfiguration& config) {
    // only options that should be specified for all types of queries
    // including views and etc..
    SetLangVer(config.LangVer);
    SetIsEnableAntlr4Parser(config.EnableAntlr4Parser || config.FeatureFlags.GetEnableAntlr4Parser());
    return *this;
}

NSQLTranslation::TTranslationSettings TKqpTranslationSettingsBuilder::Build(NYql::TExprContext& ctx) {
    NSQLTranslation::TTranslationSettings settings;
    settings.PgParser = UsePgParser && *UsePgParser;
    settings.Antlr4Parser = false;
    settings.EmitReadsForExists = true;
    settings.LangVer = LangVer;
    settings.BackportMode = BackportMode;
    if (settings.PgParser) {
        settings.AutoParametrizeEnabled = IsEnablePgConstsToParams ;
        settings.AutoParametrizeValuesStmt = IsEnablePgConstsToParams;
    }

    if (!settings.PgParser) {
        settings.Antlr4Parser = IsEnableAntlr4Parser;
    }

    if (QueryType == NYql::EKikimrQueryType::Scan || QueryType == NYql::EKikimrQueryType::Query) {
        SqlVersion = SqlVersion ? *SqlVersion : 1;
    }

    if (SqlVersion) {
        settings.SyntaxVersion = *SqlVersion;

        if (*SqlVersion > 0) {
            // Restrict fallback to V0
            settings.V0Behavior = NSQLTranslation::EV0Behavior::Disable;
        }
    } else {
        settings.SyntaxVersion = KqpYqlSyntaxVersion;
        settings.V0Behavior = NSQLTranslation::EV0Behavior::Silent;
    }

    if (IsEnableExternalDataSources) {
        settings.DynamicClusterProvider = NYql::KikimrProviderName;
        settings.BindingsMode = BindingsMode;
        settings.SaveWorldDependencies = true;
    }

    settings.PGDisable = !IsEnablePgSyntax;
    settings.InferSyntaxVersion = true;
    settings.V0ForceDisable = false;
    settings.WarnOnV0 = false;
    settings.DefaultCluster = Cluster;
    settings.ClusterMapping = {
        {Cluster, TString(NYql::KikimrProviderName)},
        {"pg_catalog", TString(NYql::PgProviderName)},
        {"information_schema", TString(NYql::PgProviderName)}
    };
    auto tablePathPrefix = KqpTablePathPrefix;
    if (!KqpTablePathPrefix.empty()) {
        settings.PathPrefix = KqpTablePathPrefix;
    }

    if (SqlAutoCommit) {
        settings.EndOfQueryCommit = *SqlAutoCommit;
    } else {
        settings.EndOfQueryCommit = QueryType == NYql::EKikimrQueryType::YqlScript || QueryType == NYql::EKikimrQueryType::YqlScriptStreaming;
    }

    settings.Flags.insert("FlexibleTypes");
    settings.Flags.insert("AnsiLike");
    if (QueryType == NYql::EKikimrQueryType::Scan
        || QueryType == NYql::EKikimrQueryType::YqlScript
        || QueryType == NYql::EKikimrQueryType::YqlScriptStreaming
        || QueryType == NYql::EKikimrQueryType::Query
        || QueryType == NYql::EKikimrQueryType::Script)
    {
        // We enable EmitAggApply for filter and aggregate pushdowns to Column Shards
        settings.Flags.insert("EmitAggApply");
    } else {
        settings.Flags.insert("DisableEmitStartsWith");
    }

    if (QueryType == NYql::EKikimrQueryType::Query || QueryType == NYql::EKikimrQueryType::Script)
    {
        settings.Flags.insert("AnsiOptionalAs");
        settings.Flags.insert("WarnOnAnsiAliasShadowing");
        settings.Flags.insert("AnsiCurrentRow");
        settings.Flags.insert("AnsiInForEmptyOrNullableItemsCollections");
    }

    if (QueryParameters) {
        NSQLTranslation::TTranslationSettings versionSettings = settings;
        NYql::TIssues versionIssues;

        if (ParseTranslationSettings(QueryText, versionSettings, versionIssues) && versionSettings.SyntaxVersion == 1) {
            for (const auto& [paramName, paramType] : *(QueryParameters)) {
                auto type = NYql::ParseTypeFromYdbType(paramType, ctx);
                if (type != nullptr) {
                    if (paramName.StartsWith("$")) {
                        settings.DeclaredNamedExprs[paramName.substr(1)] = NYql::FormatType(type);
                    } else {
                        settings.DeclaredNamedExprs[paramName] = NYql::FormatType(type);
                    }
                }
            }
        }
    }

    settings.ApplicationName = ApplicationName;
    settings.GUCSettings = GUCSettings;

    return settings;
}

NYql::TAstParseResult ParseQuery(const TString& queryText, bool isSql, TMaybe<ui16>& sqlVersion, bool& deprecatedSQL,
        NYql::TExprContext& ctx, TKqpTranslationSettingsBuilder& settingsBuilder, bool& keepInCache, TMaybe<TString>& commandTagName,
        NSQLTranslation::TTranslationSettings* effectiveSettings) {
    NYql::TAstParseResult astRes;
    settingsBuilder.SetSqlVersion(sqlVersion);
    if (isSql) {
        auto settings = settingsBuilder.Build(ctx);
        TKqpAutoParamBuilderFactory autoParamBuilderFactory;
        settings.AutoParamBuilderFactory = &autoParamBuilderFactory;
        NYql::TStmtParseInfo stmtParseInfo;

        NSQLTranslationV1::TLexers lexers;
        lexers.Antlr3 = NSQLTranslationV1::MakeAntlr3LexerFactory();
        lexers.Antlr3Ansi = NSQLTranslationV1::MakeAntlr3AnsiLexerFactory();
        lexers.Antlr4 = NSQLTranslationV1::MakeAntlr4LexerFactory();
        lexers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiLexerFactory();
        NSQLTranslationV1::TParsers parsers;
        parsers.Antlr3 = NSQLTranslationV1::MakeAntlr3ParserFactory();
        parsers.Antlr3Ansi = NSQLTranslationV1::MakeAntlr3AnsiParserFactory();
        parsers.Antlr4 = NSQLTranslationV1::MakeAntlr4ParserFactory();
        parsers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiParserFactory();

        NSQLTranslation::TTranslators translators(
            NSQLTranslationV0::MakeTranslator(),
            NSQLTranslationV1::MakeTranslator(lexers, parsers),
            NSQLTranslationPG::MakeTranslator()
        );

        auto ast = NSQLTranslation::SqlToYql(translators, queryText, settings, nullptr, &stmtParseInfo, effectiveSettings);
        deprecatedSQL = (ast.ActualSyntaxType == NYql::ESyntaxType::YQLv0);
        sqlVersion = ast.ActualSyntaxType == NYql::ESyntaxType::YQLv1 ? 1 : 0;
        keepInCache = stmtParseInfo.KeepInCache;
        commandTagName = stmtParseInfo.CommandTagName;
        return std::move(ast);
    } else {
        sqlVersion = {};
        deprecatedSQL = true;
        return NYql::ParseAst(queryText);

        // Do not check SQL constraints on s-expressions input, as it may come from both V0/V1.
        // Constraints were already checked on type annotation of SQL query.
    }
}

TQueryAst ParseQuery(const TString& queryText, const TMaybe<Ydb::Query::Syntax>& syntax, bool isSql, TKqpTranslationSettingsBuilder& settingsBuilder) {
    bool deprecatedSQL;
    bool keepInCache;
    TMaybe<TString> commandTagName;
    TMaybe<ui16> sqlVersion;
    if (syntax && *syntax == Ydb::Query::Syntax::SYNTAX_PG) {
        settingsBuilder.SetUsePgParser(true);
    }

    NYql::TExprContext ctx;
    auto astRes = ParseQuery(queryText, isSql, sqlVersion, deprecatedSQL, ctx, settingsBuilder, keepInCache, commandTagName);
    return TQueryAst(std::make_shared<NYql::TAstParseResult>(std::move(astRes)), sqlVersion, deprecatedSQL, keepInCache, commandTagName);
}

TVector<TQueryAst> ParseStatements(const TString& queryText, bool isSql, TMaybe<ui16>& sqlVersion, bool& deprecatedSQL,
        NYql::TExprContext& ctx, TKqpTranslationSettingsBuilder& settingsBuilder) {
    TVector<TQueryAst> result;
    settingsBuilder.SetSqlVersion(sqlVersion);
    NSQLTranslationV1::TLexers lexers;
    lexers.Antlr3 = NSQLTranslationV1::MakeAntlr3LexerFactory();
    lexers.Antlr3Ansi = NSQLTranslationV1::MakeAntlr3AnsiLexerFactory();
    lexers.Antlr4 = NSQLTranslationV1::MakeAntlr4LexerFactory();
    lexers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiLexerFactory();
    NSQLTranslationV1::TParsers parsers;
    parsers.Antlr3 = NSQLTranslationV1::MakeAntlr3ParserFactory();
    parsers.Antlr3Ansi = NSQLTranslationV1::MakeAntlr3AnsiParserFactory();
    parsers.Antlr4 = NSQLTranslationV1::MakeAntlr4ParserFactory();
    parsers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiParserFactory();

    NSQLTranslation::TTranslators translators(
        NSQLTranslationV0::MakeTranslator(),
        NSQLTranslationV1::MakeTranslator(lexers, parsers),
        NSQLTranslationPG::MakeTranslator()
    );

    if (isSql) {
        auto settings = settingsBuilder.Build(ctx);
        TKqpAutoParamBuilderFactory autoParamBuilderFactory;
        settings.AutoParamBuilderFactory = &autoParamBuilderFactory;
        ui16 actualSyntaxVersion = 0;
        TVector<NYql::TStmtParseInfo> stmtParseInfo;
        auto astStatements = NSQLTranslation::SqlToAstStatements(translators, queryText, settings, nullptr, &actualSyntaxVersion, &stmtParseInfo);
        deprecatedSQL = (actualSyntaxVersion == 0);
        sqlVersion = actualSyntaxVersion;
        YQL_ENSURE(astStatements.size() == stmtParseInfo.size());
        for (size_t i = 0; i < astStatements.size(); ++i) {
            result.push_back({std::make_shared<NYql::TAstParseResult>(std::move(astStatements[i])), sqlVersion, (actualSyntaxVersion == 0), stmtParseInfo[i].KeepInCache, stmtParseInfo[i].CommandTagName});
        }
        return result;
    } else {
        sqlVersion = {};
        return {{std::make_shared<NYql::TAstParseResult>(NYql::ParseAst(queryText)), sqlVersion, true, true, {}}};
    }
}

TVector<TQueryAst> ParseStatements(const TString& queryText, const TMaybe<Ydb::Query::Syntax>& syntax, bool isSql, TKqpTranslationSettingsBuilder& settingsBuilder, bool perStatementExecution) {
    if (!perStatementExecution) {
        return {ParseQuery(queryText, syntax, isSql, settingsBuilder)};
    }
    bool deprecatedSQL;
    TMaybe<ui16> sqlVersion;
    if (syntax && *syntax == Ydb::Query::Syntax::SYNTAX_PG) {
        settingsBuilder.SetUsePgParser(true);
    }

    NYql::TExprContext ctx;
    return ParseStatements(queryText, isSql, sqlVersion, deprecatedSQL, ctx, settingsBuilder);
}

} // namespace NKqp
} // namespace NKikimr
