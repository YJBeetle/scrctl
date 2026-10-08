// 非 Apple 平台按本构建的 libav 能力选择音频后端。
#include "decode/AudioDecoder.h"
#if defined(SCRCTL_HAVE_LIBAV)
#include "decode/FFmpegEldDecoder.h"
#endif

namespace scrctl {

std::unique_ptr<AudioDecoder> create_audio_decoder(int sample_rate, int channels,
                                                  int frame_length, std::string &err) {
#if defined(SCRCTL_HAVE_LIBAV)
    return create_ffmpeg_eld_decoder(sample_rate, channels, frame_length, err);
#else
    (void)sample_rate;
    (void)channels;
    (void)frame_length;
    err = SCRCTL_TR(kNoAudioDecoderMessage);
    return nullptr;
#endif
}

}  // namespace scrctl
