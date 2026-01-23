#pragma once

#include "gst/GSTElement.h"
#include <gst/app/gstappsink.h>
#include <memory>
#include <string>
#include <cstring>
#include <vector>

class VCU_Decode {
private:
    GSTElement pipeline;

    GSTElement file_source;
    GSTElement demuxer;
    GSTElement parser;
    GSTElement decoder;
    GSTElement converter;
    GSTElement app_sink;

    std::unique_ptr<std::vector<uint8_t>> frame_buffer;

    size_t target_frame  = 0;
    size_t current_frame = 0;
    bool   got_target    = false;

    int    width         = 0;
    int    height        = 0;
    double fps           = 0.0;
private:
    void create_pipeline();
    void teardown_pipeline();

    void configure_file_source(std::string const & filename);
    void configure_demuxer();
    void configure_parser();
    void configure_decoder();
    void configure_converter();
    void configure_app_sink();

    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
    static void on_pad_added(GstElement* src, GstPad* pad, gpointer user_data);

public:
    VCU_Decode();
    ~VCU_Decode();

    void set_vcu_info(int const video_width, int const video_height, double const framerate);

    void decode_frame(std::string const & filename, size_t const frame_index);
    void decode(std::string const & filename);
};
