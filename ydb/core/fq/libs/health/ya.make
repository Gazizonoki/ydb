LIBRARY()

SRCS(
    health.cpp
)

PEERDIR(
    ydb/library/actors/core
    ydb/core/fq/libs/shared_resources
    ydb/core/mon
    client/ydb_discovery
)

YQL_LAST_ABI_VERSION()

END()
