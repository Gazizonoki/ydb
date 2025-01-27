#include <ydb/public/sdk/cpp/client/ydb_query/client.h>

#include <thread>

int main() {
    NYdb::TDriver driver{NYdb::TDriverConfig{"grpc://localhost:2136/?database=/local"}};
    NYdb::NQuery::TQueryClient client{driver};

    using namespace std::chrono_literals;

    while (true) {
        auto result = client.RetryQuerySync([](NYdb::NQuery::TSession session) {
            return session.ExecuteQuery("SELECT 1", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
        });

        Cout << "Max size:" << client.GetActiveSessionsLimit() << Endl;
        Cout << "Current size:" << client.GetCurrentPoolSize() << Endl;
        Cout << "Active sessions count:" << client.GetActiveSessionCount() << Endl << Endl;

        if (!result.IsSuccess()) {
            Cout << result << Endl << Endl;
        }

        std::this_thread::sleep_for(300ms);
    }
    return 0;
}
