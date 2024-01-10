LIBRARY()

SRCS(
    result_formatter.cpp
)

PEERDIR(
    library/cpp/json
    library/cpp/json/yson
    ydb/library/mkql_proto
    ydb/library/yql/ast
    ydb/library/yql/minikql/computation
    ydb/library/yql/public/udf
    ydb/public/api/protos
    client/ydb_proto
    client/ydb_result
    client/ydb_value
    ydb/library/yql/providers/common/codec
    ydb/library/yql/providers/common/schema/expr
    ydb/library/yql/providers/common/schema/mkql
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
