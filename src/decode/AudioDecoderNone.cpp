// 非 Apple 平台的音频后端：没有。理由与实测数字写在 AudioDecoder.h 的
// kHaveAudioDecoder 上面，这里只负责把那句话在运行期也说一遍。
#include <string>

#include "decode/AudioDecoder.h"

namespace scrctl {

std::unique_ptr<AudioDecoder> create_audio_decoder(int, int, int, std::string &err) {
    err = SCRCTL_TR(kNoAudioDecoderMessage);
    return nullptr;
}

}  // namespace scrctl
