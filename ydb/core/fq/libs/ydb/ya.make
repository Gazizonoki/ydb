LIBRARY()

SRCS(
    schema.cpp
    util.cpp
    ydb.cpp
)

PEERDIR(
    ydb/library/actors/core
    library/cpp/retry
    ydb/core/base
    ydb/core/fq/libs/config
    ydb/core/fq/libs/events
    ydb/library/security
    client/ydb_coordination
    client/ydb_rate_limiter
    client/ydb_scheme
    client/ydb_table
)

GENERATE_ENUM_SERIALIZATION(ydb.h)

YQL_LAST_ABI_VERSION()

END()
