LIBRARY(transfer_workload)

SRCS(
    transfer_workload.cpp
    transfer_workload_topic_to_table.cpp
    transfer_workload_topic_to_table_init.cpp
    transfer_workload_topic_to_table_clean.cpp
    transfer_workload_topic_to_table_run.cpp
    transfer_workload_defines.cpp
)

PEERDIR(
    ydb/library/yql/public/issue
    ydb/library/yql/public/issue/protos
    ydb/public/api/grpc
    ydb/public/api/protos
    ydb/public/api/protos/annotations
    ydb/public/lib/operation_id
    ydb/public/lib/operation_id/protos
    client/draft
    client/ydb_driver
    client/ydb_proto
    client/ydb_table
    client/ydb_topic
    client/ydb_types/operation
    client/ydb_types/status    
)

END()
