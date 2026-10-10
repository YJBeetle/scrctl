#pragma once
#include <functional>
namespace scrctl::app {
struct Options;
int run_ddi_install(const Options &, const std::function<bool()> &cancelled);
}
