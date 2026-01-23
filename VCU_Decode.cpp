#include "VCU_Decode.h"
#include <iostream>
#include <fstream>

namespace {

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

}

VCU_Decode::VCU_Decode()  = default;
VCU_Decode::~VCU_Decode() = default;

void VCU_Decode::decode_frame(std::string const & filename, size_t const frame_index)
{
    target_frame  = frame_index;
    current_frame = 0;
    got_target    = false;

    decode(filename);
}

void VCU_Decode::decode(std::string const & filename)
{
    create_pipeline();

    configure_file_source( filename );
    configure_demuxer();
    configure_parser();
    configure_decoder();
    configure_converter();
    configure_app_sink();

    gst_bin_add_many(
            GST_BIN( pipeline.get() ),
            file_source.get(),
            demuxer.get(),
            parser.get(),
            decoder.get(),
            converter.get(),
            app_sink.get(),
            nullptr );

    gst_element_link_many(
            file_source.get(),
            demuxer.get(),
            nullptr );

    gst_element_link_many(
            parser.get(),
            decoder.get(),
            converter.get(),
            app_sink.get(),
            nullptr );

    /* 1. Go to PAUSED first (no frames flow yet) */
    gst_element_set_state(pipeline.get(), GST_STATE_PAUSED);
    gst_element_get_state(pipeline.get(), nullptr, nullptr,
                          GST_CLOCK_TIME_NONE);

    /* 2️. Seek BEFORE playback */
    if (target_frame > 0) {
        gint64 ts = (target_frame * GST_SECOND) / fps;
        gst_element_seek_simple(
            pipeline.get(),
            GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
            ts);
    }

    /* 3️. Reset counters AFTER seek */
    current_frame = 0;
    got_target    = false;

    /* 4️. Now start decoding */
    gst_element_set_state(
            pipeline.get(),
            GST_STATE_PLAYING );

    GstBus* bus = gst_element_get_bus( pipeline.get() );
    bool done = false;

    while ( !done ) {
        GstMessage* msg =
            gst_bus_timed_pop_filtered(
                bus,
                GST_CLOCK_TIME_NONE,
                (GstMessageType)
                (GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_APPLICATION));

        if (msg) {
            done = true;
            gst_message_unref(msg);
        }
    }

    gst_object_unref( bus );
    teardown_pipeline();
}

void VCU_Decode::create_pipeline() {
    pipeline     = make_gst_pipeline("decoding-pipeline");

    file_source  = make_gst_element("filesrc", "file-src");
    demuxer      = make_gst_element("qtdemux", "demuxer");
    parser       = make_gst_element("h264parse", "parser");
    decoder      = make_gst_element("omxh264dec", "decoder"); // software decoder for Ubuntu: avdec_h264
    converter    = make_gst_element("videoconvert", "converter");
    app_sink     = make_gst_element("appsink", "app-sink");
}

void VCU_Decode::teardown_pipeline()
{
    if ( pipeline.get() ) {
        gst_element_set_state( 
            pipeline.get(),
            GST_STATE_NULL );

        gst_object_unref( GST_PIPELINE(pipeline.get()) );
    }
}

void VCU_Decode::configure_file_source(
        std::string const & file )
{
    g_object_set(
            G_OBJECT( file_source.get() ),
            "location",
            file.c_str(),
            nullptr );
}

void VCU_Decode::configure_demuxer()
{
    g_signal_connect(
            demuxer.get(),
            "pad-added",
            G_CALLBACK( on_pad_added ),
            parser.get() );
}

void VCU_Decode::configure_parser()
{}

void VCU_Decode::configure_decoder()
{
    g_object_set(
        G_OBJECT( decoder.get() ),
        "low-latency",
        TRUE,
        nullptr );
}

void VCU_Decode::configure_converter()
{}

void VCU_Decode::configure_app_sink()
{
    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "RGB",
        nullptr
    );
    g_object_set(G_OBJECT(app_sink.get()), "caps", caps, nullptr);
    gst_caps_unref(caps);

    g_object_set(
            G_OBJECT( app_sink.get() ),
            "emit-signals", TRUE,
            "sync", FALSE,
            nullptr );

    g_signal_connect(
            app_sink.get(),
            "new-sample",
            G_CALLBACK(on_new_sample),
            this);
}

GstFlowReturn VCU_Decode::on_new_sample(GstAppSink* sink, gpointer user_data)
{
    std::cout << "on_new_smaple\n";

    auto* self = static_cast<VCU_Decode*>(user_data);
    if (!self || self->got_target)
        return GST_FLOW_OK;

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_OK;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;

    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    if (self->current_frame == self->target_frame) {

        size_t size = self->width * self->height * 3;
        self->frame_buffer =
            std::make_unique<std::vector<uint8_t>>(size);

        std::memcpy(self->frame_buffer->data(),
                    map.data,
                    size);

        save_ppm("target_frame.ppm",
                 self->frame_buffer->data(),
                 self->width,
                 self->height);

        self->got_target = true;

        GstMessage* msg = gst_message_new_application(
            GST_OBJECT(self->pipeline.get()),
            gst_structure_new_empty("FRAME_DONE"));

        gst_element_post_message(self->pipeline.get(), msg);
    }

    self->current_frame++;

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    return GST_FLOW_OK;
}

void VCU_Decode::on_pad_added(GstElement* src, GstPad* pad, gpointer user_data)
{
    GstElement* parser_elem = static_cast<GstElement*>( user_data );
    GstPad* sink_pad = gst_element_get_static_pad( parser_elem, "sink" );

    if ( !gst_pad_is_linked( sink_pad ) ) {
        gst_pad_link( pad, sink_pad );
    }

    gst_object_unref( sink_pad );
}

void VCU_Decode::set_vcu_info(int const video_width, int const video_height, double const framerate)
{
    width  = video_width;
    height = video_height;
    fps    = framerate;
}