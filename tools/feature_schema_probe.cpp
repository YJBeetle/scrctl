// 探针：把任意 CoreDevice feature 的必填键**自动问出来**。
//
// 为什么这样做而不是照着别的实现抄：设备侧是 Swift Codable，缺哪个键就点名要哪个
// （"Expected to find key includeAppClips."），类型不对也直说（"Expected to decode
// Bool but found a OS_xpc_uint64"）。所以设备本身就是最好的文档，而且拿到的是这台
// 设备这个 iOS 版本真正接受的答案。
//
// 一轮只点一个键，一条条试太慢，所以在这里循环：发一次 -> 读出缺的键名 -> 补上 ->
// 再发，直到成功或换成了别的错误。每轮只多一次 RPC，会话不重建。
#include <algorithm>
#include <cstdio>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "plist/Bplist.h"
#include "plist/Plist.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace {

using scrctl::xpc::make_bool;
using scrctl::xpc::make_dict;
using scrctl::xpc::make_string;
using scrctl::xpc::Value;

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> out;
    size_t pos = 0;
    std::string rest = s;
    while (true) {
        pos = rest.find(',');
        const auto part = rest.substr(0, pos);
        if (!part.empty()) {
            out.push_back(part);
        }
        if (pos == std::string::npos) {
            break;
        }
        rest.erase(0, pos + 1);
    }
    return out;
}

/// 从设备的错误文本里抠出它点名要的键名。
std::string missing_key(const std::string& err)
{
    static const std::string marker = "Expected to find key ";
    const auto at = err.find(marker);
    if (at == std::string::npos) {
        return {};
    }
    auto from = at + marker.size();
    const auto end = err.find_first_of(". \"", from);
    return err.substr(from, end - from);
}

/// 抠 NSCodingPath，得到"这个键该插在哪个父字典下面"。
/// 形如 `[CodingKeys(stringValue: "options", intValue: nil)]`，多层时按顺序给出。
std::vector<std::string> coding_path(const std::string& err)
{
    std::vector<std::string> path;
    static const std::string marker = "stringValue: \"";
    auto at = err.find("NSCodingPath");
    if (at == std::string::npos) {
        return path;
    }
    const auto stop = err.find(']', at);
    at = err.find(marker, at);
    while (at != std::string::npos && (stop == std::string::npos || at < stop)) {
        at += marker.size();
        const auto end = err.find('"', at);
        if (end == std::string::npos) {
            break;
        }
        path.push_back(err.substr(at, end - at));
        at = err.find(marker, end);
    }
    return path;
}

/// 沿 path 走到（并创建）那层字典，返回它的引用。path 为空就是根。
/// 半路撞上一个不是字典的东西（比如上一轮把它当布尔补上了）就就地改成字典——
/// 设备说"dictionary required here"时正是这种情况。
Value& descend(Value& root, const std::vector<std::string>& path)
{
    Value* cur = &root;
    for (const auto& key : path) {
        if (cur->type != scrctl::xpc::Type::Dict) {
            cur->type = scrctl::xpc::Type::Dict;
            cur->dict.clear();
        }
        Value* next = nullptr;
        for (auto& entry : cur->dict) {
            if (entry.key == key) {
                next = &entry.value;
                break;
            }
        }
        if (next == nullptr) {
            scrctl::xpc::dict_set(*cur, key, make_dict());
            next = &cur->dict.back().value;
        }
        cur = next;
    }
    if (cur->type != scrctl::xpc::Type::Dict) {
        cur->type = scrctl::xpc::Type::Dict;
        cur->dict.clear();
    }
    return *cur;
}

/// 把路径拼成稳定的 map 键。
std::string join_path(const std::vector<std::string>& path)
{
    std::string out;
    for (const auto& p : path) {
        if (!out.empty()) {
            out += '.';
        }
        out += p;
    }
    return out;
}

/// 设备说"Expected to decode String but found a OS_xpc_bool instead"时，
/// 从 NSCodingPath 里认出是哪个键、要什么类型。
struct TypeHint {
    std::string path;  // 空表示根，不该发生
    std::string kind;  // String / Int64 / UInt64 / Bool / ...
};

TypeHint type_hint(const std::string& err)
{
    static const std::string marker = "Expected to decode ";
    const auto at = err.find(marker);
    if (at == std::string::npos) {
        return {};
    }
    auto from = at + marker.size();
    const auto end = err.find(' ', from);
    return { join_path(coding_path(err)), err.substr(from, end - from) };
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // 用法：app_probe --service NAME --feature NAME --action NAME
    //                 [--str key=value ...] [--bool-key k1,k2] [--max-rounds N]
    std::string service = "com.apple.coredevice.appservice";
    std::string feature;
    std::string action;
    std::vector<std::pair<std::string, std::string>> strings;
    std::vector<std::pair<std::string, std::string>> opt_strings;
    int max_rounds = 12;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        }
        else if (a == "--service" && i + 1 < argc) {
            service = argv[++i];
        }
        else if (a == "--feature" && i + 1 < argc) {
            feature = argv[++i];
        }
        else if (a == "--action" && i + 1 < argc) {
            action = argv[++i];
        }
        else if (a == "--str" && i + 1 < argc) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            strings.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        else if (a == "--opt-str" && i + 1 < argc) {
            // 塞进 options 那一层的字符串键。形状收敛只补布尔，语义参数得人给。
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            opt_strings.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        else if (a == "--max-rounds" && i + 1 < argc) {
            max_rounds = std::stoi(argv[++i]);
        }
    }

    if (feature.empty()) {
        std::fprintf(stderr,
                     "用法: %s --feature NAME [--action NAME] [--str k=v] [--opt-str k=v]\n",
                     argv[0]);
        return 2;
    }
    if (action.empty()) {
        action = feature;
    }

    std::string err;
    auto dev = scrctl::remote::Device::establish({}, err, verbose);
    if (!dev) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("会话就绪：%s / iOS %s\n", dev->property("ProductType").c_str(),
                dev->property("OSVersion").c_str());

    // 已点名的键：路径 + 名字。路径来自设备的 NSCodingPath，所以嵌套字典里的键
    // 也能一路补到底。
    std::vector<std::pair<std::vector<std::string>, std::string>> found;
    // 设备说"dictionary required here"的那些路径：先建成空字典，里面的键之后补。
    std::vector<std::vector<std::string>> dicts;
    // 同理，"array required here"的那些路径建成空数组。
    std::vector<std::vector<std::string>> arrays;
    // 设备点名的类型：完整路径 -> 类型名。
    std::map<std::string, std::string> kinds;

    for (int round = 1; round <= max_rounds; ++round) {
        auto input = make_dict();
        for (const auto& [key, value] : strings) {
            scrctl::xpc::dict_set(input, key, make_string(value));
        }
        for (const auto& [key, value] : opt_strings) {
            scrctl::xpc::dict_set(descend(input, {"options"}), key, make_string(value));
        }
        for (const auto& path : dicts) {
            descend(input, path);
        }
        for (const auto& path : arrays) {
            if (path.empty()) {
                continue;
            }
            auto parent = path;
            const auto leaf = parent.back();
            parent.pop_back();
            scrctl::xpc::dict_set(descend(input, parent), leaf, scrctl::xpc::make_array());
        }
        for (const auto& [path, key] : found) {
            // 类型按设备点名的来；没点过的一律先给布尔（绝大多数开关都是布尔）。
            const auto joined = join_path(path) + "." + key;
            const auto it = kinds.find(joined);
            const std::string& kind = it == kinds.end() ? std::string("Bool") : it->second;
            Value value = make_bool(false);
            if (kind == "String") {
                value = make_string("x");
            }
            else if (kind == "Int64" || kind == "UInt64" || kind == "Int32" || kind == "UInt32") {
                value = scrctl::xpc::make_int64(0);
            }
            else if (kind == "Data") {
                // 设备对空 Data 的回话是 "Cannot parse a NULL or zero-length data"，
                // 也就是说它是一段 plist。给一个空的 bplist 字典。
                auto dict = scrctl::plist::Value::Dict();
                value = scrctl::xpc::make_data(scrctl::plist::write_binary(dict));
            }
            scrctl::xpc::dict_set(descend(input, path), key, std::move(value));
        }

        Value out;
        std::string call_err;
        if (dev->feature(service, feature, action, input, out, call_err, verbose, 90000)) {
            std::printf("\n第 %d 轮：成功\n回信: %s\n", round,
                        scrctl::xpc::describe(out).substr(0, 1500).c_str());
            std::printf("形状: %s\n", scrctl::xpc::describe(input).substr(0, 800).c_str());
            return 0;
        }

        const auto path = coding_path(call_err);
        const bool want_dict = call_err.find("dictionary required here") != std::string::npos;
        const bool want_array = call_err.find("array required here") != std::string::npos;
        if ((want_dict || want_array) && !path.empty()) {
            std::printf("第 %d 轮：%s 要是%s -> 建成%s\n", round, path.back().c_str(),
                        want_dict ? "字典" : "数组", want_dict ? "空字典" : "空数组");
            // 它不能同时又被当布尔补上，否则下一轮又把它覆盖回去了。
            std::erase_if(found, [&](const auto& e) { return e.second == path.back(); });
            (want_dict ? dicts : arrays).push_back(path);
            continue;
        }

        const auto hint = type_hint(call_err);
        if (!hint.path.empty() && !hint.kind.empty()) {
            // 类型错误的 NSCodingPath 指的是**正在解码的那个容器**，不是出错的那个
            // 字段：报 "options.user 要 String" 时，真正不对的是我们刚往 user 里补的
            // 那个键（shortName）。所以先找"父路径正好是它"的 found 条目，把类型给
            // 那个键；找不到才认为它说的是路径自己的那个叶子。
            auto target = std::find_if(found.begin(), found.end(),
                                       [&](const auto& e) { return join_path(e.first) == hint.path; });
            const std::string key_path =
                target != found.end() ? hint.path + "." + target->second : hint.path;
            std::printf("第 %d 轮：%s 要是 %s -> 换类型\n", round, key_path.c_str(),
                        hint.kind.c_str());
            if (target != found.end()) {
                kinds[key_path] = hint.kind;
                continue;
            }
            kinds[key_path] = hint.kind;
            std::erase_if(dicts, [&](const auto& p) { return join_path(p) == hint.path; });
            std::erase_if(arrays, [&](const auto& p) { return join_path(p) == hint.path; });
            std::erase_if(found, [&](const auto& e) { return join_path(e.first) == hint.path; });
            continue;
        }

        const auto key = missing_key(call_err);
        if (key.empty()) {
            std::printf("\n第 %d 轮：不再是要键的错误，停下\n%s\n", round, call_err.c_str());
            std::printf("已补上的键: ");
            for (const auto& [p, k] : found) {
                std::printf("%s%s ", p.empty() ? "" : (p.back() + ".").c_str(), k.c_str());
            }
            std::printf("\n需要是字典的路径: ");
            for (const auto& p : dicts) {
                std::printf("%s ", p.empty() ? "(根)" : p.back().c_str());
            }
            std::printf("\n");
            return 1;
        }
        std::printf("第 %d 轮：设备要 %s%s -> 补上\n", round,
                    path.empty() ? "" : (path.back() + ".").c_str(), key.c_str());
        found.emplace_back(path, key);
    }

    std::printf("到 %d 轮还没收敛\n", max_rounds);
    return 1;
}
