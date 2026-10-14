#include "media/RecordingVideoEncoder.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef SCRCTL_HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
#endif
namespace {
using Encoder = scrctl::media::RecordingVideoEncoder;
int failures = 0, checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr,"FAIL: %s\n",message); }
}
scrctl::Frame colors() {
    scrctl::Frame frame;
    frame.width=64; frame.height=96; frame.row_pitch=64*4+16;
    frame.pixels.assign(frame.row_pitch*frame.height,0);
    // BGRA red, green, blue and yellow, TL/TR/BL/BR respectively.
    const std::array<std::array<uint8_t,4>,4> color{{{0,0,255,255},{0,255,0,255},{255,0,0,255},{0,255,255,255}}};
    for (uint32_t y=0;y<frame.height;++y)
        for (uint32_t x=0;x<frame.width;++x)
            std::memcpy(frame.pixels.data()+y*frame.row_pitch+x*4,color[(y>=48)*2+(x>=32)].data(),4);
    return frame;
}
#ifdef SCRCTL_HAVE_LIBAV
int classify(const scrctl::Frame& frame, int x, int y) {
    const auto* pixel=frame.pixels.data()+y*frame.row_pitch+x*4;
    if (pixel[2]>160 && pixel[1]<80 && pixel[0]<80) return 0;
    if (pixel[1]>160 && pixel[2]<80 && pixel[0]<80) return 1;
    if (pixel[0]>160 && pixel[1]<80 && pixel[2]<80) return 2;
    if (pixel[1]>160 && pixel[2]>160 && pixel[0]<80) return 3;
    return -1;
}
void verify(const std::vector<Encoder::Packet>& packets) {
    check(packets.size()==5,"one encoded image per input with no duplicate PTS");
    const AVCodec* codec=avcodec_find_decoder_by_name("hevc");
    AVCodecContext* decoder=avcodec_alloc_context3(codec);
    check(decoder && avcodec_open2(decoder,codec,nullptr)>=0,"real decoder opens normalized HEVC");
    if (!decoder || !decoder->codec) { avcodec_free_context(&decoder); return; }
    AVFrame* raw=av_frame_alloc(); AVPacket* packet=av_packet_alloc();
    SwsContext* conversion=nullptr;
    unsigned count=0;
    const std::array<std::array<int,4>,4> expected{{{0,1,2,3},{2,0,3,1},{3,2,1,0},{1,3,0,2}}};
    auto drain=[&] {
        int received;
        while ((received=avcodec_receive_frame(decoder,raw))>=0) {
            check(raw->width==64 && raw->height==96,"every rotated/cropped image has the original fixed canvas");
            check(count<5 && raw->pts==static_cast<int64_t>(count)*800,"decoder PTS matches original variable frame input order");
            scrctl::Frame image; image.width=64; image.height=96; image.row_pitch=64*4;
            image.pixels.resize(image.row_pitch*image.height);
            conversion=sws_getCachedContext(conversion,raw->width,raw->height,static_cast<AVPixelFormat>(raw->format),
                64,96,AV_PIX_FMT_BGRA,SWS_BILINEAR,nullptr,nullptr,nullptr);
            uint8_t* output[4]{image.pixels.data(),nullptr,nullptr,nullptr};
            int stride[4]{static_cast<int>(image.row_pitch),0,0,0};
            check(conversion && sws_scale(conversion,raw->data,raw->linesize,0,96,output,stride)==96,"decoded pixels are inspectable");
            if (conversion && count<4) {
                const bool sideways=count==1||count==3;
                const int top=sideways?32:12, bottom=sideways?60:84;
                check(classify(image,8,top)==expected[count][0] && classify(image,56,top)==expected[count][1] &&
                      classify(image,8,bottom)==expected[count][2] && classify(image,56,bottom)==expected[count][3],
                      "actual decoded colored corners rotate clockwise without mirror or inverted direction");
                if (sideways) {
                    const auto* bar=image.pixels.data()+4*image.row_pitch+32*4;
                    check(bar[0]<25&&bar[1]<25&&bar[2]<25,"sideways image fits aspect ratio with black bars");
                }
            } else if (conversion && count==4) {
                check(classify(image,8,12)==0&&classify(image,56,84)==0,
                      "source crop is applied before rotation and fit");
            }
            ++count; av_frame_unref(raw);
        }
        check(received==AVERROR(EAGAIN)||received==AVERROR_EOF,"normalized HEVC pictures fully decode");
    };
    bool configured = false;
    for (const auto& encoded:packets) {
        std::vector<uint8_t> bytes;
        // VideoToolbox returns parameter sets separately (as container extradata),
        // while libx265 may also repeat them in-band. Supply the real metadata
        // when opening this raw decoder, just as the muxer does for its track.
        if (!configured) {
            check(!encoded.vps.empty() && !encoded.sps.empty() && !encoded.pps.empty(),
                  "first encoded image supplies complete decoder configuration");
            for (const auto* nal : {&encoded.vps, &encoded.sps, &encoded.pps}) {
                bytes.insert(bytes.end(), {0,0,0,1});
                bytes.insert(bytes.end(), nal->begin(), nal->end());
            }
            configured = true;
        }
        for (const auto& nal:encoded.nals) {
            bytes.insert(bytes.end(),{0,0,0,1}); bytes.insert(bytes.end(),nal.begin(),nal.end());
        }
        check(av_new_packet(packet,static_cast<int>(bytes.size()))>=0,"decode packet allocation succeeds");
        std::memcpy(packet->data,bytes.data(),bytes.size());
        packet->pts=packet->dts=encoded.pts_ticks;
        check(avcodec_send_packet(decoder,packet)>=0,"normalized recording packet enters decoder");
        av_packet_unref(packet); drain();
    }
    check(avcodec_send_packet(decoder,nullptr)>=0,"decoder flush succeeds"); drain();
    check(count==5,"all transformed pictures survive actual HEVC decoding");
    sws_freeContext(conversion); av_frame_free(&raw); av_packet_free(&packet); avcodec_free_context(&decoder);
}
#endif
}
int main() {
    std::string error;
    auto invalid=Encoder::create({63,96},error);
    check(!invalid&&!error.empty(),"odd fixed canvas rejected explicitly");
    if (!Encoder::available(error)) {
        check(!Encoder::create({64,96},error)&&!error.empty(),"missing encoder cannot silently substitute raw recording");
        std::printf("recording_video_encoder: encoder unavailable, %d checks\n",checks);
        return failures?1:0;
    }
    auto encoder=Encoder::create({64,96},error);
    check(encoder!=nullptr,"concrete fixed-canvas encoder opens");
    if (!encoder) { std::fprintf(stderr,"ENCODER: %s\n",error.c_str()); return 1; }
    auto frame=colors(); std::vector<Encoder::Packet> packets;
    for (int rotation=0;rotation<4;++rotation)
        check(encoder->encode(frame,rotation*90,{},rotation*800,packets,error),"colored image is transformed and encoded");
    check(encoder->encode(frame,0,{0,0,32,48},3200,packets,error),"crop enters encoder");
    check(encoder->finish(packets,error),"encoder flush preserves final image");
    const auto count=packets.size();
    check(encoder->finish(packets,error)&&packets.size()==count,"finish is idempotent and never duplicates packets");
#ifdef SCRCTL_HAVE_LIBAV
    verify(packets);
#endif
    for (int kind=0;kind<3;++kind) {
        auto reject=Encoder::create({64,96},error);
        check(reject!=nullptr,"invalid-input encoder opens"); if (!reject) continue;
        std::vector<Encoder::Packet> output;
        check(reject->encode(frame,0,{},0,output,error),"valid first input establishes the encoder clock");
        auto bad=frame;
        if (kind==1) bad.pixels.resize(4);
        const auto crop=kind==2?Encoder::Crop{63,0,2,96}:Encoder::Crop{};
        check(!reject->encode(bad,0,crop,kind==0?0:800,output,error)&&!error.empty(),
              "duplicate time, truncated BGRA and out-of-range crop fail explicitly");
    }
    std::printf("recording_video_encoder: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
