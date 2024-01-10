LIBRARY()

SRCS(
    common.cpp
)

PEERDIR(
    ydb/core/kqp/ut/common
    client/ydb_operation
    client/ydb_query
)

YQL_LAST_ABI_VERSION()

END()
