#pragma once

#include "remote/Device.h"
#include <optional>
#include <string>

namespace scrctl::app {

std::optional<remote::Device> open_device(const std::string &serial, const std::string &wifi,
                                          std::string &err);

} // namespace scrctl::app
