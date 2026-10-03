#pragma once

#include "app/Options.h"

namespace scrctl::app {

enum class ParseResult { Run, ExitSuccess, Error };
ParseResult parse_args(int argc, char **argv, Options &options);

} // namespace scrctl::app
