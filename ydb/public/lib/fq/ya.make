LIBRARY()

SRCS(
    fq.cpp
    scope.cpp
)

PEERDIR(
    library/cpp/json
    ydb/public/api/grpc/draft
    client/ydb_table
)

END()
