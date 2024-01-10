LIBRARY()

SRCS(
    pqv1.cpp
)

PEERDIR(
    ydb/public/api/protos
    client/ydb_persqueue_core
)

END()

RECURSE_FOR_TESTS(
    ut
)
