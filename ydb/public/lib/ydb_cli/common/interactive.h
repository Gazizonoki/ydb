#pragma once

#include <client/ydb_scheme/scheme.h>

namespace NYdb {
namespace NConsoleClient {

bool AskYesOrNo();

bool IsStdinInteractive();

bool IsStdoutInteractive();

std::optional<size_t> GetTerminalWidth();

}
}
