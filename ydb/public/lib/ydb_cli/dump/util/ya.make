LIBRARY()

SRCS(
    util.cpp
)

PEERDIR(
    ydb/public/lib/ydb_cli/common
    client/ydb_scheme
    client/ydb_table
    client/ydb_types/status
)

END()
