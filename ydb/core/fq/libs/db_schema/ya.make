LIBRARY()

SRCS(
    db_schema.cpp
)

PEERDIR(
    client/ydb_params
    client/ydb_table
)

YQL_LAST_ABI_VERSION()

END()
