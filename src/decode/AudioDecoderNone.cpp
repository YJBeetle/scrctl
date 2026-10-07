// 当前非 Apple 构建未接入音频解码后端；工厂返回空结果并提供共用提示。
// 这是本构建的能力限制，不代表该平台或其他库不能解码 AAC-ELD。
#include <string>

#include "decode/AudioDecoder.h"

namespace scrctl {

std::unique_ptr<AudioDecoder> create_audio_decoder(int, int, int, std::string &err) {
    err = SCRCTL_TR(kNoAudioDecoderMessage);
    return nullptr;
}

}  // namespace scrctl
