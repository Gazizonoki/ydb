LIBRARY()

SRCS(
    json2_udf.cpp
    kqp_ut_common.cpp
    kqp_ut_common.h
    re2_udf.cpp
    string_udf.cpp
    columnshard.cpp
    datetime2_udf.cpp
)

PEERDIR(
    ydb/core/kqp/federated_query
    ydb/core/testlib
    ydb/library/yql/public/udf
    ydb/library/yql/udfs/common/string
    ydb/library/yql/utils/backtrace
    ydb/public/lib/yson_value
    client/draft
    client/ydb_query
    client/ydb_proto
    client/ydb_scheme
    client/ydb_table
    client/ydb_topic
)

YQL_LAST_ABI_VERSION()

END()
