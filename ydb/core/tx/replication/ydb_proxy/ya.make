LIBRARY()

PEERDIR(
    ydb/core/base
    ydb/core/protos
    client/ydb_driver
    client/ydb_scheme
    client/ydb_table
    client/ydb_topic
    client/ydb_types/credentials
    client/ydb_types/credentials/login
)

SRCS(
    ydb_proxy.cpp
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
