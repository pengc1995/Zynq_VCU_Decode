#pragma once
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

class VCU_Decode {
public:
    VCU_Decode();
    ~VCU_Decode();

    void create_pipeline();
    void configure_elements(const std::string& filename);
    void configure_app_sink();
    void start_pipeline();
    void teardown_pipeline();

private:
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
    static void on_pad_added(GstElement* src, GstPad* pad, gpointer user_data);

    GstElement* pipeline = nullptr;
    GstElement* file_source = nullptr;
    GstElement* demuxer = nullptr;
    GstElement* parser = nullptr;
    GstElement* decoder = nullptr;
    GstElement* converter = nullptr;
    GstElement* app_sink = nullptr;

    std::unique_ptr<std::vector<uint8_t>> frame_buffer;
};
