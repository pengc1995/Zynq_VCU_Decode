#include "VCU_Decode.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <fstream>

void save_ppm(const std::string& filename, const uint8_t* rgb_data, int width, int height)
{
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs) {
        throw std::runtime_error("Failed to open file for writing: " + filename);
    }

    // PPM header
    ofs << "P6\n" << width << " " << height << "\n255\n";

    // RGB data
    // The data is assumed to be row-major, RGBRGB...
    ofs.write(reinterpret_cast<const char*>(rgb_data), width * height * 3);

    ofs.close();
}


VCU_Decode::VCU_Decode() {}
VCU_Decode::~VCU_Decode() { teardown_pipeline(); }

void VCU_Decode::create_pipeline() {
    pipeline     = gst_pipeline_new("decoding-pipeline");
    file_source  = gst_element_factory_make("filesrc", "file-src");
    demuxer      = gst_element_factory_make("qtdemux", "demuxer");
    parser       = gst_element_factory_make("h264parse", "parser");
    decoder      = gst_element_factory_make("omxh264dec", "decoder"); // software decoder for Ubuntu: avdec_h264
    converter    = gst_element_factory_make("videoconvert", "converter");
    app_sink     = gst_element_factory_make("appsink", "app-sink");

    gst_bin_add_many(GST_BIN(pipeline),
                     file_source,
                     demuxer,
                     parser,
                     decoder,
                     converter,
                     app_sink,
                     nullptr);

    gst_element_link_many(file_source, demuxer, nullptr);
    gst_element_link_many(parser, decoder, converter, app_sink, nullptr);

    g_signal_connect(demuxer, "pad-added", G_CALLBACK(on_pad_added), parser);
}

void VCU_Decode::configure_elements(const std::string& filename) {
    g_object_set(file_source, "location", filename.c_str(), nullptr);
}

void VCU_Decode::configure_app_sink() {
    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "RGB",
        nullptr
    );

    g_object_set(G_OBJECT(app_sink), "caps", caps, nullptr);
    gst_caps_unref(caps);

    g_object_set(app_sink,
                 "emit-signals", TRUE,
                 "sync", FALSE,
                 nullptr);

    // connect to class static callback
    g_signal_connect(app_sink, "new-sample",
                     G_CALLBACK(on_new_sample),
                     this);
}

GstFlowReturn VCU_Decode::on_new_sample(GstAppSink* sink, gpointer user_data)
{
    int width = 0, height = 0;

    auto* self = static_cast<VCU_Decode*>(user_data);
    if (!self) return GST_FLOW_OK;

    // Pull the sample from appsink
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    // Get the caps to detect width/height
    GstCaps* caps = gst_sample_get_caps(sample);
    if (!caps) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstStructure* s = gst_caps_get_structure(caps, 0);
    gst_structure_get_int(s, "width", &width);
    gst_structure_get_int(s, "height", &height);

    if (width <= 0 || height <= 0) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // Optional: round up to multiple of 16 for VCU-safe buffer
    int padded_width  = (width  + 15) / 16 * 16;
    int padded_height = (height + 15) / 16 * 16;

    size_t expected_size = padded_width * padded_height * 3;

    // Allocate buffer if not yet allocated or size changed
    if (!self->frame_buffer || self->frame_buffer->size() != expected_size) {
        self->frame_buffer = std::make_unique<std::vector<uint8_t>>(expected_size);
    }

    // Get the actual buffer
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // Copy only the real width*height RGB data (skip any padded rows)
    for (int row = 0; row < height; ++row) {
        std::memcpy(
            self->frame_buffer->data() + row * width * 3,
            map.data + row * padded_width * 3,
            width * 3
        );
    }

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    static bool saved = false;
    if (!saved) {
        save_ppm("last_frame.ppm", self->frame_buffer->data(), width, height);
        saved = true;
    }

    return GST_FLOW_OK;
}


void VCU_Decode::on_pad_added(GstElement* src, GstPad* pad, gpointer user_data) {
    GstElement* parser_elem = static_cast<GstElement*>(user_data);
    GstPad* sink_pad = gst_element_get_static_pad(parser_elem, "sink");

    if (!gst_pad_is_linked(sink_pad)) {
        gst_pad_link(pad, sink_pad);
    }

    gst_object_unref(sink_pad);
}

void VCU_Decode::start_pipeline() {
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GstBus* bus = gst_element_get_bus(pipeline);
    bool done = false;

    while (!done) {
        GstMessage* msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
            (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

        if (msg) {
            switch (GST_MESSAGE_TYPE(msg)) {
                case GST_MESSAGE_ERROR:
                    GError* err;
                    gchar* dbg;
                    gst_message_parse_error(msg, &err, &dbg);
                    std::cerr << "Error: " << err->message << std::endl;
                    g_error_free(err);
                    g_free(dbg);
                    done = true;
                    break;
                case GST_MESSAGE_EOS:
                    std::cout << "EOS reached" << std::endl;
                    done = true;
                    break;
                default: break;
            }
            gst_message_unref(msg);
        }
    }

    gst_object_unref(bus);
    teardown_pipeline();
}

void VCU_Decode::teardown_pipeline() {
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = nullptr;
    }
}
