#pragma once

#include "i18n/Translation.h"
#include <CLI/CLI.hpp>
#include <cstdio>
#include <string>

namespace scrctl::i18n {

/// CLI11 入口共用的语言选项与帮助翻译。参数解析和退出状态仍由入口决定。
class CliLanguage {
  public:
    explicit CliLanguage(CLI::App &app) : app_(app) {
        initialize();
        option_ = app_.add_option("--lang", requested_,
            SCRCTL_N_("Message language: auto, en, zh-CN (default: environment, fallback: en)"))
            ->check(CLI::IsMember({"auto", "en", "zh-CN"}));
    }

    // CLI11 的选项绑定 requested_；对象地址需保持不变直到解析和帮助处理完成。
    CliLanguage(const CliLanguage &) = delete;
    CliLanguage &operator=(const CliLanguage &) = delete;

    /// CLI11 收集完参数后才抛出帮助请求；从原始 results 选择语言，
    /// 使 --help 前后的 --lang 均有效，包括尚未执行值转换的帮助路径。
    bool select() {
        const auto &values = option_->results();
        const auto requested = values.empty() ? std::string("auto") : values.back();
        if (requested != "auto" && requested != "en" && requested != "zh-CN") {
            std::fprintf(stderr, "%s\n", SCRCTL_TR("--lang must be auto, en or zh-CN"));
            return false;
        }
        if (!initialize(requested)) {
            std::fprintf(stderr, "%s\n", SCRCTL_TR(
                "Cannot enable the requested message locale; using English"));
        }
        return true;
    }

    std::string help() {
        app_.description(SCRCTL_TR(app_.get_description().c_str()));
        app_.footer(SCRCTL_TR(app_.get_footer().c_str()));
        for (auto *option : app_.get_options()) {
            const auto description = option->get_description();
            option->description(SCRCTL_TR(description.c_str()));
            if (option->get_group() == "OPTIONS") {
                option->group(SCRCTL_TR("Options"));
            }
        }
        auto formatter = app_.get_formatter();
        formatter->label("Usage", SCRCTL_TR("Usage"));
        formatter->label("Options", SCRCTL_TR("Options"));
        formatter->label("OPTIONS", SCRCTL_TR("Options"));
        formatter->label("Positionals", SCRCTL_TR("Positionals"));
        formatter->label("POSITIONALS", SCRCTL_TR("Positionals"));
        return app_.help();
    }

  private:
    CLI::App &app_;
    std::string requested_ = "auto";
    CLI::Option *option_ = nullptr;
};

} // namespace scrctl::i18n
