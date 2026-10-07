#pragma once

#include "i18n/Translation.h"
#include <CLI/CLI.hpp>
#include <cstdio>
#include <optional>
#include <string>

namespace scrctl::i18n {

/// CLI11 入口共用的语言选项、帮助翻译与简单探针解析入口。
/// 入口仍负责业务校验和实际退出；特殊错误码可继续使用 select() 和 help()。
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

    /// 用于参数错误固定返回 2 的入口。nullopt 表示继续执行；返回 0 或 2 时，
    /// 帮助或错误已输出，调用方应返回该退出码。解析异常之外的异常仍交给调用方。
    std::optional<int> parse(int argc, char **argv) {
        try {
            app_.parse(argc, argv);
            if (!select()) return 2;
        } catch (const CLI::CallForHelp &) {
            if (!select()) return 2;
            std::printf("%s", help().c_str());
            return 0;
        } catch (const CLI::ParseError &error) {
            if (select())
                std::fprintf(stderr, SCRCTL_TR("Invalid arguments: %s\n"), error.what());
            return 2;
        }
        return std::nullopt;
    }

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
