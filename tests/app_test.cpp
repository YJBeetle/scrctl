// launchapplication 请求体的离线自检。
//
// 为什么必须有：这条 RPC 的键放错位置时，设备**不给字段级报错**，而是退到别的分支
// 说一句无关的话。实测把 bundle id 塞进 options，回的是 "A URL to open must be
// specified in the launch options."——照着这句话去找"该给哪个 url 键"会一路找错，
// 因为真正缺的是顶层的 applicationSpecifier。所以形状只能钉在这里。
#include <cstdio>
#include <string>
#include <vector>

#include "plist/Plist.h"
#include "remote/App.h"
#include "xpc/XpcValue.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

}  // namespace

int main() {
    using scrctl::remote::App;
    using namespace scrctl::xpc;

    constexpr const char *kBundle = "com.apple.mobilesafari";
    auto input = App::build_launch(kBundle, /*terminate_existing = */ true);

    std::printf("== bundle id 的入口 ==\n");
    const auto *spec = input.find("applicationSpecifier");
    check(spec != nullptr && spec->is_dict(), "顶层有 applicationSpecifier 字典");
    const auto *bid = spec == nullptr ? nullptr : spec->find("bundleIdentifier");
    check(bid != nullptr && bid->is_dict(), "  里面是 bundleIdentifier case");
    const auto *zero = bid == nullptr ? nullptr : bid->find("_0");
    // 带关联值的枚举 case 就是 {_0: 值}，少这一层设备认不出这个 specifier。
    check(zero != nullptr && zero->as_string_or() == kBundle, "  _0 就是 bundle id");
    // 反过来说：options 里出现任何"看起来像指定要启动什么"的键，都是走回头路。
    const auto *options = input.find("options");
    check(options != nullptr && options->is_dict(), "options 是字典");
    if (options != nullptr) {
        for (const char *wrong : {"bundleIdentifier", "url", "applicationURL", "path"}) {
            check(options->find(wrong) == nullptr,
                  std::string("options 里没有 ") + wrong + "（那个位置不生效）");
        }
    }

    std::printf("== options 的必填项 ==\n");
    check(options != nullptr && options->find("terminateExisting") != nullptr &&
                  options->at("terminateExisting").as_bool_or(),
          "terminateExisting 跟着参数走（true = 先杀掉在跑的实例）");
    if (const auto *user = options == nullptr ? nullptr : options->find("user");
        user != nullptr) {
        check(user->at("shortName").as_string_or() == "mobile", "user.shortName=mobile");
    } else {
        check(false, "有 user 字典");
    }
    // 这条是实测换来的：零长 Data 被拒（"Cannot parse a NULL or zero-length data"），
    // 所以哪怕内容什么都没有，也得是一段解析得动的 plist。
    const auto *pso = options == nullptr ? nullptr : options->find("platformSpecificOptions");
    check(pso != nullptr && pso->type == Type::Data && !pso->data.empty(),
          "platformSpecificOptions 是非空 Data");
    if (pso != nullptr && !pso->data.empty()) {
        const std::string text(pso->data.begin(), pso->data.end());
        std::string perr;
        const auto parsed = scrctl::plist::parse(text, &perr);
        check(parsed.has_value() && parsed->is_dict(), "  而且是一段能解开的 plist 字典");
    }
    check(options != nullptr && options->at("arguments").is_array(), "arguments 是数组");
    check(options != nullptr && options->at("environmentVariables").is_dict(),
          "environmentVariables 是字典");
    check(input.find("standardIOIdentifiers") != nullptr &&
                  input.at("standardIOIdentifiers").is_dict(),
          "standardIOIdentifiers 是顶层字典（不在 options 里）");

    std::printf("== 参数化 ==\n");
    const auto keep = App::build_launch(kBundle, /*terminate_existing = */ false);
    check(!keep.at("options").at("terminateExisting").as_bool_or(true),
          "terminate_existing=false 时该项为假（与 Android 的 monkey -p 语义对齐）");

    std::printf(Failures == 0 ? "\n全部通过\n" : "\n%d 项失败\n", Failures);
    return Failures == 0 ? 0 : 1;
}
