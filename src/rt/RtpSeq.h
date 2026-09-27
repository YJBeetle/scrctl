#pragma once

#include <cstdint>
#include <vector>

namespace scrctl::rt {

/// RTP 序号记账：一个序号进来，判它相对**已见过的最高水位**是接上、跳了、还是迟到。
///
/// 为什么要抽成这么一个小东西：同一段 mod 2^16 的算术在视频拆包器（`rt/RtpHevc`）与
/// 音频腿（`media/AudioPump`）里各写了一遍，而两边都犯过同一个错——把 `last_seq` 更新成
/// "最后**到达**的那个序号"，而不是"见过的**最高**序号"。这条链路上乱序到达是实测常态
/// （一秒 100 个包的音频流里每 2 秒就有几次），于是 100、102、101、103 这样一个都没丢的
/// 序列会被记成两次缺口：101 迟到把水位从 102 拽回 101，103 于是"往前跳了一格"。
/// `seq_lost` 从此虚增，而它在视频那条腿上是**触发 PLI 甚至重起会话**的理由——账错会把
/// 恢复动作引到不存在的丢包上。
///
/// 抽出来之后只测一次就够：音频泵那一份要真机才跑得着，让它直接继承被测过的这一份。
class RtpSeq {
public:
    enum class Verdict {
        kFirst,    ///< 这一轮第一个包：没有可比的水位，只建水位
        kInOrder,  ///< 正好接在水位后面（forward == 1）
        kGap,      ///< 往前跳了一格以上，中间那几个序号没到
        kLate,     ///< 不超过水位：迟到的旧包，或者重复包
    };

    /// 迟到多少个序号以内还算"这一跳能被补回来"。超出就剔出欠账表、坐实为丢失。
    /// 取 1024 是个纯内存/宽容度的界：这条链路实测的乱序跨度是个位数（音频一秒 100 个包，
    /// 1024 号等于 10 秒），再大的"迟到"更可能是换了流，把它算成丢包才是保守而如实的。
    /// 它是公开的，因为 `lost()` 这个口径离不开它——看统计的人要知道冲销有期限。
    static constexpr uint32_t kReorderWindow = 1024;

    Verdict observe(uint16_t seq);

    /// 换了一条会话（重起）之后水位要重起，否则第一包会被判成几千个丢包。
    /// 注意 `lost()` 跟着归零：要跨会话累计的调用方得自己在 reset 之前把旧值收走
    /// （`media/AudioPump` 里那个 `lost_carry` 就是干这个的）。
    void reset() {
        have_ = false;
        high_ = 0;
        pending_.clear();
        detected_ = 0;
        filled_ = 0;
    }

    /// 已见过的最高序号。RTCP 接收报告里"highest sequence number"要的就是这个数，
    /// 不是"最后收到的那个"——这两者在乱序到达时是不一样的。
    [[nodiscard]] uint16_t high() const { return high_; }

    /// **真正没到**的包数：累计检测到的缺口减去后来迟到补齐的。
    ///
    /// 口径要分清，因为它决定这个数能不能拿去和别的统计对照：`observe` 返回的 `kGap`
    /// 是**事件**数（一跳可能带掉好几个号），`gaps_detected()` 是**号**数（只增），
    /// 而 `lost()` 才是"现在看，几个包确实没了"。RTCP 接收报告里的 cumulative lost
    /// 是第三个口径，所以 `app/main.cpp` 打的那行"丢包"只能对这一个。
    [[nodiscard]] uint64_t lost() const { return detected_ - filled_; }

    /// 累计"往前跳过几个号"，只增不减。它和 `lost()` 的差就是被迟到包补上的量，
    /// 两个都给出去才看得出"报了缺口但其实一个没丢"这种事。
    [[nodiscard]] uint64_t gaps_detected() const { return detected_; }

private:
    /// 还欠着的序号（检测到的洞里还没补到的那些）。只为"迟到能不能冲销"服务。
    /// 里面的号按登记顺序在水位下方排开，条数被 `kReorderWindow` 天然卡住：水位只在
    /// `observe` 里往前走，每走一次就剔掉落后超过一个窗口的洞，所以这个表不会有重复的号、
    /// 也不会长过 kReorderWindow 条。
    void remember_holes(uint16_t from, uint32_t count);
    /// 剔掉落后水位超过 `kReorderWindow` 的洞。**剔掉不会让 `lost()` 变小**——
    /// `detected_` 早就记上了，这一步只是收内存，顺便给这个号"判死"。
    void prune();

    bool have_ = false;
    uint16_t high_ = 0;
    std::vector<uint16_t> pending_;
    uint64_t detected_ = 0;
    uint64_t filled_ = 0;
};

}  // namespace scrctl::rt
