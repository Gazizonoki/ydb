LIBRARY()

SRCS(
    ydb_clickhouse_internal.cpp
    ydb_logstore.cpp
)

PEERDIR(
    ydb/core/scheme
    ydb/public/api/grpc/draft
    client/ydb_proto
    client/ydb_table
)

END()
