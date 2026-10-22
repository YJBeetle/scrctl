#include "app/Application.h"
#ifdef _WIN32
#include <CLI/CLI.hpp>
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    // The CRT's narrow argv can lose Unicode before the CLI parser sees it.
    CLI::App arguments;
    argv = arguments.ensure_utf8(argv);
#endif
    return scrctl::app::run(argc, argv);
}
