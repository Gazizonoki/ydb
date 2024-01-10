#pragma once

#include "command.h"
#include "formats.h"

#include <client/ydb_result/result.h>
#include <client/ydb_types/status/status.h>

namespace NYdb {
namespace NConsoleClient {

class TInterruptibleCommand {
protected:
    static void OnTerminate(int);
    static void SetInterruptHandlers();
    static bool IsInterrupted();

private:
    static TAtomic Interrupted;
};

}
}
