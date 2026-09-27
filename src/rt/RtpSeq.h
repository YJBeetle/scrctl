#pragma once

#include <cstdint>

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

    /// 记一个序号。@param lost 可为 nullptr；`kGap` 时写入这一跳缺了多少个包。
    Verdict observe(uint16_t seq, uint32_t *lost = nullptr);

    /// 换了一条会话（重起）之后水位要重起，否则第一包会被判成几千个丢包。
    void reset() {
        have_ = false;
        high_ = 0;
    }

    /// 已见过的最高序号。RTCP 接收报告里"highest sequence number"要的就是这个数，
    /// 不是"最后收到的那个"——这两者在乱序到达时是不一样的。
    [[nodiscard]] uint16_t high() const { return high_; }

private:
    bool have_ = false;
    uint16_t high_ = 0;
};

}  // namespace scrctl::rt
