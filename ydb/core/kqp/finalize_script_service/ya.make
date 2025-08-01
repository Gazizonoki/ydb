LIBRARY()

SRCS(
    kqp_check_script_lease_actor.cpp
    kqp_finalize_script_actor.cpp
    kqp_finalize_script_service.cpp
)

PEERDIR(
    ydb/core/kqp/counters
    ydb/core/kqp/proxy_service
    ydb/core/tx/scheme_cache
    ydb/library/table_creator
    ydb/library/yql/providers/s3/actors_factory
)

YQL_LAST_ABI_VERSION()

END()
