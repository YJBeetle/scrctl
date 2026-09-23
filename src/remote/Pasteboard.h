#pragma once

#include <memory>
#include <string>

#include "remote/Rsd.h"

namespace scrctl::remote {

class Device;

/// 设备剪贴板。
///
/// 两个用处：给自动化"粘贴"一段文本（键盘那条路只覆盖 US 布局的 ASCII，中文
/// 必须走这里），以及把设备上复制出来的东西读回来。
///
/// `pasteboardservice` 不吃 CoreDevice 外壳，也不吃 dtuhidd 那套
/// `{messageType, payload, featureIdentifier}`——消息本身就是一个字典，
/// `command` 字段直接驱动分发：
///
/// ```text
/// 写: {command:"SET",  pasteboardName:"general",
///      items:[{types:["public.utf8-plain-text"],
///              data:{"public.utf8-plain-text":{data:<裸字节>}}}]}   -> SET_REPLY
/// 读: {command:"PULL", pasteboardName:"general", dataPolicy:{allResolved:{}}}
///                                                                  -> PULL_REPLY
/// ```
///
/// 两个必须照做的规矩，都是实测/参考实现撞出来的：
///
/// 1. **一条连接只够一次要回信的请求。** dtpasteboardd 在同一个连接上收到第二个
///    带 WANTING_REPLY 的消息时会 abort（"Attempted to send non-reply msg on the
///    reply channel"），所以 set/get 各开一条新连接。
/// 2. **`types` 必须填。** 只给 `data` 而 `types` 为空时，设备会回 SET_REPLY 表示
///    "收下了"，但 PULL 回来的条目里 data 是空的——静默丢弃，比报错更难查。
///
/// `data` 里的字节是原生 XPC Data，不是 base64。
class Pasteboard {
public:
    /// 一次操作一条连接：见上面第 1 条。
    static bool set_text(Device &device, const std::string &text, std::string &err,
                         bool verbose = false);
    /// 读回纯文本。设备上没复制过东西 / 只有图片时返回 false 并给出原因。
    static bool get_text(Device &device, std::string &out, std::string &err,
                         bool verbose = false);

    /// 构造 SET 的消息体。单独拆出来是为了能离线自检：形状错了设备的表现是
    /// "回个 SET_REPLY 然后把内容丢掉"，从线上根本看不出来。
    [[nodiscard]] static xpc::Value build_set(const std::string &text);
    /// 构造 PULL 的消息体。
    [[nodiscard]] static xpc::Value build_pull();
    /// 从 PULL_REPLY 里取纯文本；没有该表示形式时返回 nullptr。
    [[nodiscard]] static const std::string *find_text(const xpc::Value &reply);
};

}  // namespace scrctl::remote
