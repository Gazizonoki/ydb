UNITTEST_FOR(yql/essentials/sql/v1)

SRCS(
    sql_ut_antlr4.cpp
    sql_match_recognize_ut.cpp
)

PEERDIR(
    library/cpp/regex/pcre
    yql/essentials/public/udf/service/exception_policy
    yql/essentials/core/sql_types
    yql/essentials/sql
    yql/essentials/sql/pg_dummy
    yql/essentials/sql/v1/format
    yql/essentials/sql/v1/lexer/antlr4
)

TIMEOUT(300)

SIZE(MEDIUM)

END()
