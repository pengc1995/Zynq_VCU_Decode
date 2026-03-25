#pragma once

#include "gst/GSTElement.h"
#include "gst/GSTMainLoop.h"
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
    GSTElement caps_filter;
    GSTElement decoder;
    GSTElement converter;
    GSTElement app_sink;

    size_t target_frame     = 0;
    GstClockTime target_pts = 0;

    size_t current_frame = 0;
    bool   got_target    = false;

    int    width         = 0;
    int    height        = 0;
    double fps           = 0.0;

    std::unique_ptr<std::vector<uint8_t>> frame_buffer;

private:
    void create_pipeline();
    void teardown_pipeline();

    void configure_file_source(std::string const & filename);
    void configure_demuxer();
    void configure_parser();
    void configure_caps_filter();
    void configure_decoder();
    void configure_converter();
    void configure_app_sink();

    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
    static void on_pad_added(GstElement* src, GstPad* pad, gpointer user_data);
    static gboolean bus_callback(GstBus* bus, GstMessage* msg, gpointer user_data);

public:
    VCU_Decode();
    ~VCU_Decode();

    void decode(
            std::string  const & filename,
            int          const   video_width,
            int          const   video_height,
            double       const   framerate,
            unsigned int const   frame_index);
};
