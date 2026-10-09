#include "i18n/Translation.h"
#include "app/AudioOut.h"

#include <cstdio>
#include <cstring>

namespace scrctl::app {

bool AudioOut::open(scrctl::media::AudioPump &pump, std::string &err) {
    if (!pump.decoding_enabled()) {
        err = SCRCTL_TR("Audio playback is unavailable because PCM decoding is disabled");
        return false;
    }
    pump_ = &pump;
    channels_ = pump.channels() > 0 ? pump.channels() : 2;
    preroll_ = pump.preroll_frames();
    SDL_AudioSpec want{};
    want.freq = static_cast<int>(pump.sample_rate());
    want.format = AUDIO_S16SYS;
    want.channels = static_cast<Uint8>(channels_);
    // 请求每次回调 1024 帧，48 kHz 下约为 21 ms，兼顾调度开销和缓冲延迟。
    // SDL 可调整实际帧数，结果记录在 have 中。
    want.samples = 1024;
    want.callback = &AudioOut::fill;
    want.userdata = this;
    SDL_AudioSpec have{};
    // 应用回调始终提交流所用的格式；allowed_changes=0 时 SDL 可在内部
    // 转换为硬件格式，因此 have 的格式不等同于硬件的原生格式。
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev_ == 0) {
        pump_ = nullptr;
        err = SDL_GetError();
        return false;
    }
    if (have.freq != want.freq || have.channels != want.channels || have.format != want.format) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
        pump_ = nullptr;
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      SCRCTL_TR(
                          "Audio device returned %d Hz / %d channels / format 0x%x; stream requires %d Hz "
                          "/ %d channels / 0x%x. Direct playback unavailable"),
                      have.freq, have.channels, static_cast<unsigned>(have.format), want.freq,
                      want.channels, static_cast<unsigned>(want.format));
        err = buf;
        return false;
    }
    started_.store(false, std::memory_order_relaxed);
    SDL_PauseAudioDevice(dev_, 0);
    std::printf(SCRCTL_TR("Audio output opened: %d Hz / %d channels, backend %s, callback %u frames (%u bytes)\n"),
                have.freq, have.channels, SDL_GetCurrentAudioDriver(),
                static_cast<unsigned>(have.samples), static_cast<unsigned>(have.size));
    return true;
}

void AudioOut::close() {
    if (dev_ != 0) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
    pump_ = nullptr;
}

void AudioOut::fill(void *userdata, Uint8 *stream, int len) {
    auto *self = static_cast<AudioOut *>(userdata);
    const std::size_t frame_bytes = sizeof(int16_t) * static_cast<std::size_t>(self->channels_);
    const std::size_t frames = frame_bytes == 0 ? 0 : static_cast<std::size_t>(len) / frame_bytes;
    auto *dst = reinterpret_cast<int16_t *>(stream);
    if (self->pump_ == nullptr || frames == 0) {
        std::memset(dst, 0, static_cast<std::size_t>(len));
        return;
    }
    // 首次播放前等待预缓冲水位。开始播放后不再重新预缓冲；欠载时填充静音，
    // 以免每次短暂缺包都增加播放延迟。静音填充计数用于观察欠载。
    if (!self->started_.load(std::memory_order_relaxed) &&
        self->pump_->buffered_frames() < self->preroll_) {
        std::memset(dst, 0, frames * frame_bytes);
        self->silence_.fetch_add(frames, std::memory_order_relaxed);
        self->preroll_silence_.fetch_add(frames, std::memory_order_relaxed);
        return;
    }
    self->started_.store(true, std::memory_order_relaxed);
    const std::size_t got = self->pump_->read(dst, frames);
    if (got < frames) {
        std::memset(dst + got * self->channels_, 0, (frames - got) * frame_bytes);
        self->silence_.fetch_add(frames - got, std::memory_order_relaxed);
        self->underrun_silence_.fetch_add(frames - got, std::memory_order_relaxed);
        self->underrun_callbacks_.fetch_add(1, std::memory_order_relaxed);
    }
    self->delivered_.fetch_add(got, std::memory_order_relaxed);
}

} // namespace scrctl::app
