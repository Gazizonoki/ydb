#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/proto/accessor.h>

namespace NKikimr {
namespace NKqp {

using namespace NYdb;
using namespace NYdb::NTable;

namespace {
    void CreateTestTable(TSession& session) {
        AssertSuccessResult(session.ExecuteSchemeQuery(R"(
            --!syntax_v1

            CREATE TABLE TestImmediateEffects (
                Key Uint64,
                Value String,
                PRIMARY KEY (Key)
            );
        )").GetValueSync());

        auto result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            INSERT INTO TestImmediateEffects (Key, Value) VALUES
                (1u, "One"),
                (2u, "Two");
        )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
    }

    void CreateShardedTestTable(TSession& session) {
        AssertSuccessResult(session.ExecuteSchemeQuery(R"(
            --!syntax_v1

            CREATE TABLE TestImmediateEffects (
                Key Uint64,
                Value String,
                PRIMARY KEY (Key)
            ) WITH (
                PARTITION_AT_KEYS = (100, 200)
            );
        )").GetValueSync());

        auto result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            INSERT INTO TestImmediateEffects (Key, Value) VALUES
                (1u, "Value1"),
                (2u, "Value2"),
                (100u, "Value100"),
                (200u, "Value200");
        )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
    }
}  // namespase

Y_UNIT_TEST_SUITE(KqpImmediateEffects) {
    Y_UNIT_TEST(Upsert) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Three"),
                    (4u, "Four");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
        }

        {  // multiple effects
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (5u, "Five");
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (6u, "Six");
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (7u, "Seven");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]];
                [[5u];["Five"]];
                [[6u];["Six"]];
                [[7u];["Seven"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(UpsertDuplicates) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "SomeValue1"),
                    (3u, "SomeValue2");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["SomeValue2"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
        }
    }

    Y_UNIT_TEST(UpsertExistingKey) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (1u, "SomeValue1");
                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["SomeValue1"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
        }

        {  // multiple effects
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (1u, "SomeValue11");
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (2u, "SomeValue2");
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (2u, "SomeValue22");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["SomeValue11"]];
                [[2u];["SomeValue22"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(Replace) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                REPLACE INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Three"),
                    (4u, "Four");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
        }

        {  // multiple effects
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                REPLACE INTO TestImmediateEffects (Key, Value) VALUES (5u, "Five");
                REPLACE INTO TestImmediateEffects (Key, Value) VALUES (6u, "Six");
                REPLACE INTO TestImmediateEffects (Key, Value) VALUES (7u, "Seven");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]];
                [[5u];["Five"]];
                [[6u];["Six"]];
                [[7u];["Seven"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(ReplaceDuplicates) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                REPLACE INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "SomeValue1"),
                    (3u, "SomeValue2");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["SomeValue2"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
        }
    }

    Y_UNIT_TEST(ReplaceExistingKey) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                REPLACE INTO TestImmediateEffects (Key, Value) VALUES (1u, "SomeValue1");
                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["SomeValue1"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
        }

        {  // multiple effects
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                REPLACE INTO TestImmediateEffects (Key, Value) VALUES (1u, "SomeValue11");
                REPLACE INTO TestImmediateEffects (Key, Value) VALUES (2u, "SomeValue2");
                REPLACE INTO TestImmediateEffects (Key, Value) VALUES (2u, "SomeValue22");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["SomeValue11"]];
                [[2u];["SomeValue22"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(Insert) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                INSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Three"),
                    (4u, "Four");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
        }

        {  // multiple effects
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                INSERT INTO TestImmediateEffects (Key, Value) VALUES (5u, "Five");
                INSERT INTO TestImmediateEffects (Key, Value) VALUES (6u, "Six");
                INSERT INTO TestImmediateEffects (Key, Value) VALUES (7u, "Seven");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]];
                [[5u];["Five"]];
                [[6u];["Six"]];
                [[7u];["Seven"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST_TWIN(InsertDuplicates, UseSink) {
        TKikimrSettings serverSettings;
        serverSettings.AppConfig.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                INSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Three"),
                    (3u, "SomeValue");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
            UNIT_ASSERT(HasIssue(result.GetIssues(), NYql::TIssuesIds::KIKIMR_CONSTRAINT_VIOLATION, [](const auto& issue) {
                return issue.GetMessage().contains(UseSink ? "Conflict with existing key." : "Duplicated keys found.");
            }));
        }
    }

    Y_UNIT_TEST_TWIN(InsertExistingKey, UseSink) {
        TKikimrSettings serverSettings;
        serverSettings.AppConfig.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                INSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (2u, "SomeValue");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
            UNIT_ASSERT(HasIssue(result.GetIssues(), NYql::TIssuesIds::KIKIMR_CONSTRAINT_VIOLATION, [](const auto& issue) {
                return issue.GetMessage().contains("Conflict with existing key.");
            }));
        }
    }

    Y_UNIT_TEST(UpdateOn) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                UPDATE TestImmediateEffects ON (Key, Value) VALUES
                    (1u, "Updated1"),
                    (2u, "Updated2");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["Updated1"]];
                [[2u];["Updated2"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
        }

        {  // multiple effects
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                UPDATE TestImmediateEffects ON (Key, Value) VALUES
                    (1u, "Updated3"),
                    (2u, "Updated4");

                UPDATE TestImmediateEffects ON (Key, Value) VALUES
                    (1u, "Updated5");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Updated5"]];
                [[2u];["Updated4"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(Delete) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                DELETE FROM TestImmediateEffects WHERE Key = 2;

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["One"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
        }

        {  // multiple effects
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Three"),
                    (4u, "Four");

                DELETE FROM TestImmediateEffects WHERE Key > 3;
                DELETE FROM TestImmediateEffects WHERE Key < 3;

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[3u];["Three"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(UpdateAfterUpsert) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        auto result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            UPSERT INTO TestImmediateEffects (Key, Value) VALUES (3u, "Three");
            UPSERT INTO TestImmediateEffects (Key, Value) VALUES (4u, "Four");

            UPDATE TestImmediateEffects SET Value = "Updated2" WHERE Key = 2;
            UPDATE TestImmediateEffects SET Value = "Updated3" WHERE Key = 3;

            SELECT * FROM TestImmediateEffects;
        )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([
            [[1u];["One"]];
            [[2u];["Updated2"]];
            [[3u];["Updated3"]];
            [[4u];["Four"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(DeleteAfterUpsert) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        auto result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            UPSERT INTO TestImmediateEffects (Key, Value) VALUES (3u, "Three");
            UPSERT INTO TestImmediateEffects (Key, Value) VALUES (4u, "Four");

            DELETE FROM TestImmediateEffects WHERE Key = 2;
            DELETE FROM TestImmediateEffects WHERE Key = 3;

            SELECT * FROM TestImmediateEffects;

            UPSERT INTO TestImmediateEffects (Key, Value) VALUES (2u, "Value2");
            UPSERT INTO TestImmediateEffects (Key, Value) VALUES (3u, "Value3");

            SELECT * FROM TestImmediateEffects;
        )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([
            [[1u];["One"]];
            [[4u];["Four"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));
        CompareYson(R"([
            [[1u];["One"]];
            [[2u];["Value2"]];
            [[3u];["Value3"]];
            [[4u];["Four"]]
        ])", FormatResultSetYson(result.GetResultSet(1)));
    }

    Y_UNIT_TEST(UpdateAfterInsert) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        auto result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            INSERT INTO TestImmediateEffects (Key, Value) VALUES (3u, "Three");
            INSERT INTO TestImmediateEffects (Key, Value) VALUES (4u, "Four");

            UPDATE TestImmediateEffects SET Value = "Updated2" WHERE Key = 2;
            UPDATE TestImmediateEffects SET Value = "Updated3" WHERE Key = 3;

            SELECT * FROM TestImmediateEffects;
        )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([
            [[1u];["One"]];
            [[2u];["Updated2"]];
            [[3u];["Updated3"]];
            [[4u];["Four"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(DeleteAfterInsert) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);


        auto result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            INSERT INTO TestImmediateEffects (Key, Value) VALUES (3u, "Three");
            INSERT INTO TestImmediateEffects (Key, Value) VALUES (4u, "Four");

            DELETE FROM TestImmediateEffects WHERE Key = 2;
            DELETE FROM TestImmediateEffects WHERE Key = 3;

            SELECT * FROM TestImmediateEffects;

            INSERT INTO TestImmediateEffects (Key, Value) VALUES (2u, "Two");
            INSERT INTO TestImmediateEffects (Key, Value) VALUES (3u, "Three");

            SELECT * FROM TestImmediateEffects;
        )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([
            [[1u];["One"]];
            [[4u];["Four"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));
        CompareYson(R"([
            [[1u];["One"]];
            [[2u];["Two"]];
            [[3u];["Three"]];
            [[4u];["Four"]]
        ])", FormatResultSetYson(result.GetResultSet(1)));
    }

    Y_UNIT_TEST(UpsertAfterInsert) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        auto result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            INSERT INTO TestImmediateEffects (Key, Value) VALUES (3u, "Three");
            UPSERT INTO TestImmediateEffects (Key, Value) VALUES (3u, "NewValue3");

            SELECT * FROM TestImmediateEffects;
        )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([
            [[1u];["One"]];
            [[2u];["Two"]];
            [[3u];["NewValue3"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(UpsertAfterInsertWithIndex) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSampleTablesWithIndex(session);

        {  // secondary key
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                INSERT INTO SecondaryKeys (Key, Fk, Value) VALUES
                    (6u, 6u, "Payload6");

                UPSERT INTO SecondaryKeys (Key, Fk, Value) VALUES
                    (6u, 60u, "Payload60");

                SELECT * FROM SecondaryKeys VIEW Index;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [#;#;["Payload8"]];
                [#;[7];["Payload7"]];
                [[1];[1];["Payload1"]];
                [[2];[2];["Payload2"]];
                [[5];[5];["Payload5"]];
                [[60];[6];["Payload60"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }

        {  // secondary complex keys
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                INSERT INTO SecondaryComplexKeys (Key, Fk1, Fk2, Value) VALUES
                    (8u, 8u, "Fk8", "Payload8");

                UPSERT INTO SecondaryComplexKeys (Key, Fk1, Fk2) VALUES
                    (8u, 8u, "Fk9");

                SELECT * FROM SecondaryComplexKeys VIEW Index;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [#;#;#;["Payload8"]];
                [#;["Fk7"];[7];["Payload7"]];
                [[1];["Fk1"];[1];["Payload1"]];
                [[2];["Fk2"];[2];["Payload2"]];
                [[5];["Fk5"];[5];["Payload5"]];
                [[8];["Fk9"];[8];["Payload8"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }

        {  // secondary index with data column
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                INSERT INTO SecondaryWithDataColumns (Key, Index2, Value) VALUES
                    ("Primary2", "Secondary2", "Value2");

                UPSERT INTO SecondaryWithDataColumns (Key, Index2, Value) VALUES
                    ("Primary2", "Secondary22", "Value22");

                SELECT * FROM SecondaryWithDataColumns VIEW Index;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [#;["Secondary1"];["Primary1"];["Value1"]];
                [#;["Secondary22"];["Primary2"];["Value22"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(DeleteOnAfterInsertWithIndex) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSampleTablesWithIndex(session);

        {  // secondary key
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM SecondaryKeys;

                INSERT INTO SecondaryKeys (Key, Fk, Value) VALUES
                    (6u, 6u, "Payload6");

                DELETE FROM SecondaryKeys ON (Key) VALUES (6u);

                SELECT * FROM SecondaryKeys;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(FormatResultSetYson(result.GetResultSet(0)), FormatResultSetYson(result.GetResultSet(1)));
        }

        {  // secondary complex keys
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM SecondaryComplexKeys VIEW Index;

                INSERT INTO SecondaryComplexKeys (Key, Fk1, Fk2, Value) VALUES
                    (8u, 8u, "Fk8", "Payload8");

                DELETE FROM SecondaryComplexKeys ON (Key) VALUES (8u);

                SELECT * FROM SecondaryComplexKeys VIEW Index;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(FormatResultSetYson(result.GetResultSet(0)), FormatResultSetYson(result.GetResultSet(1)));
        }

        {  // secondary index with data column
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM SecondaryWithDataColumns VIEW Index;

                INSERT INTO SecondaryWithDataColumns (Key, Index2, Value) VALUES
                    ("Primary2", "Secondary2", "Value2");

                DELETE FROM SecondaryWithDataColumns ON (Key) VALUES ("Primary2");

                SELECT * FROM SecondaryWithDataColumns VIEW Index;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(FormatResultSetYson(result.GetResultSet(0)), FormatResultSetYson(result.GetResultSet(1)));
        }
    }

    Y_UNIT_TEST(MultipleEffectsWithIndex) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSampleTablesWithIndex(session);

        auto result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            SELECT * FROM SecondaryKeys VIEW Index;

            INSERT INTO SecondaryKeys (Key, Fk, Value) VALUES
                (10u, 10u, "Payload10");

            UPSERT INTO SecondaryKeys (Key, Fk, Value) VALUES
                (20u, 20u, "Payload20");

            SELECT * FROM SecondaryKeys VIEW Index;

            UPDATE SecondaryKeys ON (Key, Fk) VALUES
                (20u, 21u);

            UPDATE SecondaryKeys SET Fk = 20u WHERE Key = 20u;

            SELECT * FROM SecondaryKeys VIEW Index;

            DELETE FROM SecondaryKeys ON (Key) VALUES (20u);

            DELETE FROM SecondaryKeys ON (Key) VALUES (10u);

            SELECT * FROM SecondaryKeys VIEW Index;
        )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(FormatResultSetYson(result.GetResultSet(0)), FormatResultSetYson(result.GetResultSet(3)));
        CompareYson(FormatResultSetYson(result.GetResultSet(1)), FormatResultSetYson(result.GetResultSet(2)));
    }

    Y_UNIT_TEST(InsertConflictTxAborted) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Value3"),
                    (101u, "Value101");

                INSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "NewValue3"),
                    (201u, "Value201");
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects ORDER BY Key;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]];
                [[2u];["Value2"]];
                [[100u];["Value100"]];
                [[200u];["Value200"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(UpsertConflictInteractiveTxAborted) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx;
        {
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Value3"),
                    (101u, "Value101");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx = result.GetTransaction();
            UNIT_ASSERT(tx);
        }

        {
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects ORDER BY Key;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]];
                [[2u];["Value2"]];
                [[100u];["Value100"]];
                [[200u];["Value200"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }

        {
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "NewValue3");
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {
            auto result = tx->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects ORDER BY Key;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]];
                [[2u];["Value2"]];
                [[3u];["Value3"]];
                [[100u];["Value100"]];
                [[101u];["Value101"]];
                [[200u];["Value200"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(MultiShardUpsertAfterRead) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session);

        std::optional<TTransaction> tx;
        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects ORDER BY Key;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx = result.GetTransaction();
            UNIT_ASSERT(tx);
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Value3"),
                    (101u, "Value101");
            )", TTxControl::Tx(*tx).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST_TWIN(TxWithReadAtTheEnd, UseSink) {
        TKikimrSettings serverSettings;
        serverSettings.AppConfig.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Full);

        auto result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                (3u, "Value3"),
                (101u, "Value101"),
                (201u, "Value201");

            UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                (4u, "Value4"),
                (101u, "NewValue101");

            SELECT * FROM TestImmediateEffects ORDER BY Key;
        )", TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([
            [[1u];["Value1"]];
            [[2u];["Value2"]];
            [[3u];["Value3"]];
            [[4u];["Value4"]];
            [[100u];["Value100"]];
            [[101u];["NewValue101"]];
            [[200u];["Value200"]];
            [[201u];["Value201"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
        // check that last (commit) phase is empty
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(stats.query_phases().size() - 1).table_access().size(), 0);
    }

    Y_UNIT_TEST_TWIN(InteractiveTxWithReadAtTheEnd, UseSink) {
        TKikimrSettings serverSettings;
        serverSettings.AppConfig.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Full);

        std::optional<TTransaction> tx;
        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects ORDER BY Key;

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Value3"),
                    (101u, "Value101"),
                    (201u, "Value201");
            )", TTxControl::BeginTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]];
                [[2u];["Value2"]];
                [[100u];["Value100"]];
                [[200u];["Value200"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx = result.GetTransaction();
            UNIT_ASSERT(tx);
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (4u, "Value4");

                SELECT * FROM TestImmediateEffects ORDER BY Key;
            )", TTxControl::Tx(*tx).CommitTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]];
                [[2u];["Value2"]];
                [[3u];["Value3"]];
                [[4u];["Value4"]];
                [[100u];["Value100"]];
                [[101u];["Value101"]];
                [[200u];["Value200"]];
                [[201u];["Value201"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseSink ? 4 : 5);
            // check that last (commit) phase is empty
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(stats.query_phases().size() - 1).table_access().size(), 0);
        }
    }

    Y_UNIT_TEST_TWIN(TxWithWriteAtTheEnd, UseSink) {
        TKikimrSettings serverSettings;
        serverSettings.AppConfig.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Full);

        auto result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                (3u, "Value3"),
                (101u, "Value101"),
                (201u, "Value201");

            SELECT * FROM TestImmediateEffects ORDER BY Key;

            UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                (4u, "Value4"),
                (101u, "NewValue101");
        )", TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([
            [[1u];["Value1"]];
            [[2u];["Value2"]];
            [[3u];["Value3"]];
            [[100u];["Value100"]];
            [[101u];["Value101"]];
            [[200u];["Value200"]];
            [[201u];["Value201"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
        // check that last (commit) phase contains write operation
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(stats.query_phases().size() - 1).table_access().size(), 1);
    }

    Y_UNIT_TEST(InteractiveTxWithWriteAtTheEnd) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Full);

        std::optional<TTransaction> tx;
        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects ORDER BY Key;

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Value3"),
                    (101u, "Value101"),
                    (201u, "Value201");
            )", TTxControl::BeginTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]];
                [[2u];["Value2"]];
                [[100u];["Value100"]];
                [[200u];["Value200"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx = result.GetTransaction();
            UNIT_ASSERT(tx);
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects ORDER BY Key;

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (4u, "Value4");
            )", TTxControl::Tx(*tx).CommitTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]];
                [[2u];["Value2"]];
                [[3u];["Value3"]];
                [[100u];["Value100"]];
                [[101u];["Value101"]];
                [[200u];["Value200"]];
                [[201u];["Value201"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            // check that last (commit) phase contains write operation
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(stats.query_phases().size() - 1).table_access().size(), 1);
        }
    }

    Y_UNIT_TEST(UnobservedUncommittedChangeConflict) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session);

        std::optional<TTransaction> tx1;
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        {
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (2u, "Value2Modified");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        std::optional<TTransaction> tx2;
        auto session2 = db.CreateSession().GetValueSync().GetSession();
        {
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects ORDER BY Key;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]];
                [[2u];["Value2"]];
                [[100u];["Value100"]];
                [[200u];["Value200"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (2u, "Value2MoreModified");
            )", TTxControl::Tx(*tx2)).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {
            auto result = tx2->Commit().ExtractValueSync();
            // UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::ABORTED, result.GetIssues().ToString());
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects ORDER BY Key;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]];
                [[2u];["Value2Modified"]];
                [[100u];["Value100"]];
                [[200u];["Value200"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(AlreadyBrokenImmediateEffects) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session);

        std::optional<TTransaction> tx1;
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        {
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 100u;

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "Value1Modified");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        std::optional<TTransaction> tx2;
        auto session2 = db.CreateSession().GetValueSync().GetSession();
        {
            // This just establishes a snapshot that is before tx1 commit
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 100u;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * From TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2)).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }

        {
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "Value1Impossible");
            )", TTxControl::Tx(*tx2)).ExtractValueSync();
            // UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::ABORTED, result.GetIssues().ToString());
        }

        {
            auto result = tx2->Commit().ExtractValueSync();
            // UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::ABORTED, result.GetIssues().ToString());
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects ORDER BY Key;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1Modified"]];
                [[2u];["Value2"]];
                [[100u];["Value100"]];
                [[200u];["Value200"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(WriteThenReadWithCommit) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session);

        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_EXECUTER, NLog::PRI_DEBUG);

        auto session1 = db.CreateSession().GetValueSync().GetSession();
        std::optional<TTransaction> tx1;
        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "ModifiedValue1");
            )", TTxControl::Tx(*tx1)).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx1).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["ModifiedValue1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(ConflictingKeyR1WR2) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx1;
        std::optional<TTransaction> tx2;

        {  // read1
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {  // write2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {  // commit1
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {  // read2 + commit2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["NewValue1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(ConflictingKeyR1RWR2) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx1;
        std::optional<TTransaction> tx2;

        {  // read1
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {  // read2 + write2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {  // commit1
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {  // read2 + commit2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["NewValue1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(ConflictingKeyR1WRR2) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx1;
        std::optional<TTransaction> tx2;

        {  // read1
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {  // write2 + read2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["NewValue1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {  // commit1
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {  // read2 + commit2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["NewValue1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(ConflictingKeyW1RR2) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx1;
        std::optional<TTransaction> tx2;

        {  // write1
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {  // read2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {  // commit1
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {  // read2 + commit2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(ConflictingKeyW1WR2) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx1;
        std::optional<TTransaction> tx2;

        {  // write1
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {  // write2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue11");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {  // commit1
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {  // read2 + commit2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["NewValue11"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(ConflictingKeyW1RWR2) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx1;
        std::optional<TTransaction> tx2;

        {  // write1
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {  // read2 + write2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {  // commit1
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {  // read2 + commit2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::ABORTED, result.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(ConflictingKeyW1WRR2) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx1;
        std::optional<TTransaction> tx2;

        {  // write1
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {  // write2 + read2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["NewValue1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {  // commit1
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {  // read2 + commit2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::ABORTED, result.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(ConflictingKeyRW1RR2) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx1;
        std::optional<TTransaction> tx2;

        {  // read1 + write1
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {  // read2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {  // commit1
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {  // read2 + commit2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(ConflictingKeyRW1WR2) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx1;
        std::optional<TTransaction> tx2;

        {  // read1 + write1
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {  // write2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue11");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {  // commit1
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {  // read2 + commit2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(ConflictingKeyRW1RWR2) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx1;
        std::optional<TTransaction> tx2;

        {  // read1 + write1
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {  // read2 + write2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue11");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {  // commit1
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {  // read2 + commit2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::ABORTED, result.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(ConflictingKeyRW1WRR2) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session1 = db.CreateSession().GetValueSync().GetSession();
        auto session2 = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session1);

        std::optional<TTransaction> tx1;
        std::optional<TTransaction> tx2;

        {  // read1 + write1
            auto result = session1.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["Value1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx1 = result.GetTransaction();
            UNIT_ASSERT(tx1);
        }

        {  // write2 + read2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue11");
                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["NewValue11"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            tx2 = result.GetTransaction();
            UNIT_ASSERT(tx2);
        }

        {  // commit1
            auto result = tx1->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {  // read2 + commit2
            auto result = session2.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects WHERE Key = 1u;
            )", TTxControl::Tx(*tx2).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::ABORTED, result.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST_TWIN(ForceImmediateEffectsExecution, UseSink) {
        TKikimrSettings serverSettings = TKikimrSettings().SetEnableForceImmediateEffectsExecution(true);
        serverSettings.AppConfig.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateShardedTestTable(session);

        std::optional<TTransaction> tx;
        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Full);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (1u, "NewValue1");
            )", TTxControl::BeginTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            tx = result.GetTransaction();
            UNIT_ASSERT(tx);

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            // compute phase + effect phase
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseSink ? 1 : 2);

            if (!UseSink) {
                const auto& literalPhase = stats.query_phases(0);
                UNIT_ASSERT_VALUES_EQUAL(literalPhase.table_access().size(), 0);
            }

            const auto& effectPhase = stats.query_phases(UseSink ? 0 : 1);
            UNIT_ASSERT_VALUES_EQUAL(effectPhase.table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(effectPhase.table_access(0).name(), "/Root/TestImmediateEffects");
            UNIT_ASSERT_VALUES_EQUAL(effectPhase.table_access(0).updates().rows(), 1);
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                DELETE FROM TestImmediateEffects WHERE Key = 1;
            )", TTxControl::Tx(*tx), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            // compute phase + effect phase
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseSink ? 1 : 2);

            if (!UseSink) {
                const auto& literalPhase = stats.query_phases(0);
                UNIT_ASSERT_VALUES_EQUAL(literalPhase.table_access().size(), 0);
            }

            const auto& effectPhase = stats.query_phases(UseSink ? 0 : 1);
            UNIT_ASSERT_VALUES_EQUAL(effectPhase.table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(effectPhase.table_access(0).name(), "/Root/TestImmediateEffects");
            UNIT_ASSERT_VALUES_EQUAL(effectPhase.table_access(0).deletes().rows(), 1);
        }

        {
            auto result = tx->Commit().ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(ImmediateUpdate) {
        TKikimrRunner kikimr;
        auto client = kikimr.GetTableClient();
        auto session = client.CreateSession().GetValueSync().GetSession();
        {
            const auto query = Q_(R"(
                CREATE TABLE t
                (
                    id Uint64,
                    val String,
                    created_on Uint64,
                    PRIMARY KEY(id)
                );
            )");

            auto result = session.ExecuteSchemeQuery(query).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }
        {
            const auto query = Q_(R"(
                UPSERT INTO t (id, val, created_on) VALUES
                (123, 'xxx', 1);
            )");
            auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {
            const auto query = Q_(R"(
                UPDATE t SET created_on = 11;
                UPDATE t SET val = 'abc' WHERE created_on = 11;
            )");
            auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

            result = session.ExecuteDataQuery(R"(
                SELECT * FROM t;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([[[11u];[123u];["abc"]]])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(ImmediateUpdateSelect) {
        TKikimrRunner kikimr;
        auto client = kikimr.GetTableClient();
        auto session = client.CreateSession().GetValueSync().GetSession();
        {
            const auto query = Q_(R"(
                CREATE TABLE t
                (
                    id Uint64,
                    val String,
                    created_on Uint64,
                    PRIMARY KEY(id)
                );
            )");

            auto result = session.ExecuteSchemeQuery(query).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }
        {
            const auto query = Q_(R"(
                UPSERT INTO t (id, val, created_on) VALUES
                (123, 'xxx', 1);
            )");
            auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {
            const auto query = Q_(R"(
                UPDATE t SET created_on = 11;
                UPDATE t SET val = 'abc' WHERE created_on = 11;
                SELECT * FROM t;
            )");
            auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([[[11u];[123u];["abc"]]])", FormatResultSetYson(result.GetResultSet(0)));

            result = session.ExecuteDataQuery(R"(
                SELECT * FROM t;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([[[11u];[123u];["abc"]]])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(ManyFlushes) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Three"),
                    (4u, "Four");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
        }

        {  // multiple effects
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (5u, "Five");
                SELECT * FROM TestImmediateEffects;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (6u, "Six");
                SELECT * FROM TestImmediateEffects;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (7u, "Seven");
                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]];
                [[5u];["Five"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]];
                [[5u];["Five"]];
                [[6u];["Six"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]];
                [[5u];["Five"]];
                [[6u];["Six"]];
                [[7u];["Seven"]]
            ])", FormatResultSetYson(result.GetResultSet(2)));
        }
    }

    Y_UNIT_TEST(Interactive) {
        TKikimrSettings serverSettings;
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateTestTable(session);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES
                    (3u, "Three"),
                    (4u, "Four");

                SELECT * FROM TestImmediateEffects;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (5u, "Five");
            )", TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

            auto tx = result.GetTransaction();

            result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT * FROM TestImmediateEffects;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (6u, "Six");
                SELECT * FROM TestImmediateEffects;
                UPSERT INTO TestImmediateEffects (Key, Value) VALUES (7u, "Seven");
                SELECT * FROM TestImmediateEffects;
            )", TTxControl::Tx(*tx)).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]];
                [[5u];["Five"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]];
                [[5u];["Five"]];
                [[6u];["Six"]]
            ])", FormatResultSetYson(result.GetResultSet(1)));
            CompareYson(R"([
                [[1u];["One"]];
                [[2u];["Two"]];
                [[3u];["Three"]];
                [[4u];["Four"]];
                [[5u];["Five"]];
                [[6u];["Six"]];
                [[7u];["Seven"]]
            ])", FormatResultSetYson(result.GetResultSet(2)));

            auto commitResult = tx->Commit().ExtractValueSync();
            UNIT_ASSERT(commitResult.IsSuccess());
        }
    }
}

} // namespace NKqp
} // namespace NKikimr