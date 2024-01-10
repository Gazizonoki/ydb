UNITTEST_FOR(ydb/services/ydb)

FORK_SUBTESTS()
SPLIT_FACTOR(7)

IF (SANITIZER_TYPE == "thread" OR WITH_VALGRIND)
    TIMEOUT(3600)
    SIZE(LARGE)
    TAG(ya:fat)
ELSE()
    TIMEOUT(300)
    SIZE(MEDIUM)
ENDIF()

SRCS(
    ydb_table_split_ut.cpp
)

PEERDIR(
    contrib/libs/apache/arrow
    library/cpp/getopt
    ydb/library/grpc/client
    library/cpp/regex/pcre
    library/cpp/svnversion
    ydb/core/kqp/ut/common
    ydb/core/testlib/default
    ydb/core/grpc_services/base
    ydb/core/testlib
    ydb/library/yql/minikql/dom
    ydb/library/yql/minikql/jsonpath
    ydb/public/lib/experimental
    ydb/public/lib/json_value
    ydb/public/lib/yson_value
    client/draft
    client/ydb_coordination
    client/ydb_export
    client/ydb_extension
    client/ydb_operation
    client/ydb_scheme
    client/ydb_monitoring
    ydb/services/ydb
)

YQL_LAST_ABI_VERSION()

REQUIREMENTS(ram:14)

END()
