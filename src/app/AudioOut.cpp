#include "app/AudioOut.h"

#include <cstdio>
#include <cstring>

namespace scrctl::app {

bool AudioOut::open(scrctl::media::AudioPump &pump, std::string &err) {
    pump_ = &pump;
    channels_ = pump.channels() > 0 ? pump.channels() : 2;
    preroll_ = pump.preroll_frames();
    SDL_AudioSpec want{};
    want.freq = static_cast<int>(pump.sample_rate());
    want.format = AUDIO_S16SYS;
    want.channels = static_cast<Uint8>(channels_);
    // 一次回调 1024 帧 ≈ 48kHz 下 21ms。更小会被系统追不上（xrun），更大只是
    // 把延迟搬到设备侧的缓冲里。SDL 允许就近挑，实际值在 have 里。
    want.samples = 1024;
    want.callback = &AudioOut::fill;
    want.userdata = this;
    SDL_AudioSpec have{};
    // 不传 SDL_AUDIO_ALLOW_FREQUENCY_CHANGE：我们要送的是 48kHz 的样本，而这里没有
    // 重采样器。让系统把设备开成 44.1kHz 只意味着同样的样本以 0.92 倍速放出去，
    // 现场表现是**音调低半档**——一个没人会往"出口协商"上想的症状。宁可开不了设备
    // 并说人话（调用方会退回"只收不放"）。声道数与采样格式同理不能迁就。
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
                      "声卡只肯给 %d Hz / %d 声道 / 格式 0x%x，而这条流是 %d Hz / "
                      "%d 声道 / 0x%x——这里没有重采样器",
                      have.freq, have.channels, static_cast<unsigned>(have.format), want.freq,
                      want.channels, static_cast<unsigned>(want.format));
        err = buf;
        return false;
    }
    SDL_PauseAudioDevice(dev_, 0);
    std::printf("音频出口已开：%d Hz / %d 声道，后端 %s\n", have.freq, have.channels,
                SDL_GetCurrentAudioDriver());
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
    // 只在这个"还没开口"的判据上用缓冲水位；一旦开口就不再重新攒——中途发现
    // 欠载就补静音继续，比停下来重新攒 50ms 更接近"实时"，代价是最坏情况下
    // 一段接缝的咔声，而那正好是 silence 这一位要报出来的东西。
    if (!self->started_.load(std::memory_order_relaxed) &&
        self->pump_->buffered_frames() < self->preroll_) {
        std::memset(dst, 0, frames * frame_bytes);
        self->silence_.fetch_add(frames, std::memory_order_relaxed);
        return;
    }
    self->started_.store(true, std::memory_order_relaxed);
    const std::size_t got = self->pump_->read(dst, frames);
    if (got < frames) {
        std::memset(dst + got * self->channels_, 0, (frames - got) * frame_bytes);
        self->silence_.fetch_add(frames - got, std::memory_order_relaxed);
    }
    self->delivered_.fetch_add(got, std::memory_order_relaxed);
}

} // namespace scrctl::app
