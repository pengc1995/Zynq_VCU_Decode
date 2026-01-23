#include "VCU_Decode.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <stdexcept>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

// Minimal helper: grab one RGB frame from a video
std::vector<uint8_t> grab_frame_ffmpeg(
    const std::string& filename,
    int target_frame,
    int& width,
    int& height)
{
    avformat_network_init();

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filename.c_str(), nullptr, nullptr) < 0)
        throw std::runtime_error("Failed to open video");

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0)
        throw std::runtime_error("Failed to get stream info");

    int stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0)
        throw std::runtime_error("No video stream found");

    AVStream* stream = fmt_ctx->streams[stream_index];

    AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    avcodec_open2(codec_ctx, codec, nullptr);

    width = codec_ctx->width;
    height = codec_ctx->height;

    // Seek to the nearest keyframe before target frame
    int fps = codec_ctx->framerate.num / codec_ctx->framerate.den;
    int64_t ts = av_rescale_q(target_frame, {1, fps}, stream->time_base);
    av_seek_frame(fmt_ctx, stream_index, ts, AVSEEK_FLAG_BACKWARD);

    AVPacket pkt;
    av_init_packet(&pkt);

    AVFrame* frame = av_frame_alloc();
    AVFrame* rgb_frame = av_frame_alloc();

    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
    std::vector<uint8_t> buffer(num_bytes);
    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer.data(),
                         AV_PIX_FMT_RGB24, width, height, 1);

    SwsContext* sws_ctx = sws_getContext(width, height, codec_ctx->pix_fmt,
                                         width, height, AV_PIX_FMT_RGB24,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);

    int current_frame = 0;
    bool got_frame = false;

    while (av_read_frame(fmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index != stream_index) {
            av_packet_unref(&pkt);
            continue;
        }

        avcodec_send_packet(codec_ctx, &pkt);
        av_packet_unref(&pkt);

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            if (current_frame == target_frame) {
                sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
                          rgb_frame->data, rgb_frame->linesize);
                got_frame = true;
                break;
            }
            current_frame++;
        }

        if (got_frame) break;
    }

    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    avformat_network_deinit();

    if (!got_frame)
        throw std::runtime_error("Target frame not found");

    return buffer;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <video_file.mp4>" << " target-frame-index" << std::endl;
        return 1;
    }
    std::string const filename = argv[1];
    int const target_index = std::stoi(argv[2]);


    AVFormatContext* fmt = nullptr;

    if (avformat_open_input(&fmt, filename.c_str() , nullptr, nullptr) < 0) {
        std::cerr << "Cannot open file\n";
        return -1;
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        std::cerr << "Cannot find stream info\n";
        return -1;
    }

    int vstream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vstream = i;
            break;
        }
    }

    if (vstream < 0) {
        std::cerr << "No video stream\n";
        return -1;
    }

    auto* p = fmt->streams[vstream]->codecpar;

    std::string codec;
    if (p->codec_id == AV_CODEC_ID_H264) codec = "H.264";
    else if (p->codec_id == AV_CODEC_ID_HEVC) codec = "H.265";
    else codec = "Other";

    double fps = 0.0;
    if (fmt->streams[vstream]->avg_frame_rate.num != 0) {
        fps = av_q2d(fmt->streams[vstream]->avg_frame_rate);  // frames per second
    }

    gst_init(nullptr, nullptr);

    VCU_Decode decoder;
    decoder.set_vcu_info( p->width, p->height, fps );
    decoder.decode_frame(filename, target_index);

    avformat_close_input(&fmt);

    // int width, height;
    // std::vector<uint8_t> frame = grab_frame_ffmpeg( filename, target_index, width, height );

    // std::cout << "Grabbed frame" << target_index << ": " << width << "x" << height
    //               << ", buffer size = " << frame.size() << "\n";

    //     // Optional: save as PPM to verify
    //     std::string target_name = "frame" + std::to_string(target_index) + ".ppm";
    //     std::ofstream ofs(target_name.c_str(), std::ios::binary);
    //     ofs << "P6\n" << width << " " << height << "\n255\n";
    //     ofs.write(reinterpret_cast<char*>(frame.data()), frame.size());
    //     ofs.close();
    //     std::cout << "Saved "<< target_name << ".ppm\n";
    
    std::cout << "End" << std::endl;

    return 0;
}

