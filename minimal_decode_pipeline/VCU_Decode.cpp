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

void save_ppm_from_rgba(const std::string& filename, const uint8_t* rgba_data, int width, int height)
{
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs) {
        throw std::runtime_error("Failed to open file for writing: " + filename);
    }

    // PPM header
    ofs << "P6\n" << width << " " << height << "\n255\n";

    // Write RGB only, skip alpha
    for (int i = 0; i < width * height; ++i) {
        ofs.put(rgba_data[i*4 + 0]); // R
        ofs.put(rgba_data[i*4 + 1]); // G
        ofs.put(rgba_data[i*4 + 2]); // B
        // skip A
    }

    ofs.close();
}

}

VCU_Decode::VCU_Decode()  = default;
VCU_Decode::~VCU_Decode() = default;

void VCU_Decode::decode(
        std::string  const & filename,
        int          const   video_width,
        int          const   video_height,
        double       const   framerate,
        unsigned int const   frame_index)
{
    std::cout << "decode\n";

    width  = video_width;
    height = video_height;
    fps    = framerate;

    target_frame = frame_index;

    create_pipeline();

    configure_file_source( filename );
    configure_demuxer();
    configure_parser();
    configure_caps_filter();
    configure_decoder();
    configure_converter();
    configure_app_sink();

    gst_bin_add_many(
            GST_BIN( pipeline.get() ),
            file_source.get(),
            demuxer.get(),
            parser.get(),
            caps_filter.get(),
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
            caps_filter.get(),
            decoder.get(),
            converter.get(),
            app_sink.get(),
            nullptr );

    gst_element_set_state(
            pipeline.get(),
            GST_STATE_PAUSED );
    gst_element_get_state(
        pipeline.get(),
        nullptr,
        nullptr,
        GST_CLOCK_TIME_NONE
    );

    auto loop {
        make_main_loop( static_cast<GMainContext*>( nullptr ), static_cast<gboolean>( false ) )
    };

    GstBus* bus = gst_element_get_bus( pipeline.get() );
    gst_bus_add_watch( bus, bus_callback, loop.get() );
    gst_object_unref( bus );

    target_pts = gst_util_uint64_scale( target_frame, GST_SECOND, fps );
    gst_element_seek(
        pipeline.get(),
        1.0,
        GST_FORMAT_TIME,
        static_cast<GstSeekFlags>( GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT ),
        GST_SEEK_TYPE_SET,
        target_pts,
        GST_SEEK_TYPE_NONE,
        GST_CLOCK_TIME_NONE );

    current_frame = 0;
    got_target    = false;

    gst_element_set_state(
            pipeline.get(),
            GST_STATE_PLAYING );

    g_main_loop_run( loop.get() );

    teardown_pipeline();
}

void VCU_Decode::create_pipeline() {
    pipeline     = make_gst_pipeline("decoding-pipeline");

    file_source  = make_gst_element("filesrc", "file-src");
    demuxer      = make_gst_element("qtdemux", "demuxer");
    parser       = make_gst_element("h265parse", "parser");
    caps_filter       = make_gst_element( "capsfilter", "caps-filter" );
    decoder      = make_gst_element("omxh265dec", "decoder"); // software decoder for Ubuntu: avdec_h264 // omxh265dec
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

void VCU_Decode::configure_caps_filter()
{
    // g_object_set(
    //     G_OBJECT( caps_filter.get() ),
    //     "caps",
    //     gst_caps_new_simple(
    //         "video/x-h265",
    //         "alignment", G_TYPE_STRING, "nal",
    //         nullptr ),
    //     nullptr );
}

void VCU_Decode::configure_decoder()
{
    // g_object_set(
    //     G_OBJECT( decoder.get() ),
    //     "low-latency",
    //     TRUE,
    //     nullptr );
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
    auto* self = static_cast<VCU_Decode*>(user_data);
    if (!self || self->got_target) {
        return GST_FLOW_OK;
    }

    std::cout << "current_frame: " << self->current_frame << "\n";

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_OK;
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if ( !buffer ) {
        gst_sample_unref( sample );
        return GST_FLOW_OK;
    }

    GstClockTime pts = GST_BUFFER_PTS( buffer );
    // if ( pts >= self->target_pts ) {
        GstMapInfo map;
        if ( !gst_buffer_map( buffer, &map, GST_MAP_READ ) ) {
            gst_sample_unref( sample );
            return GST_FLOW_OK;
        }

        size_t size = self->width * self->height * 3;
        self->frame_buffer =
            std::make_unique<std::vector<uint8_t>>(size);

        std::memcpy(
                self->frame_buffer->data(),
                map.data,
                size);

        std::string const savename = "frame" + std::to_string(self->current_frame) + ".ppm";
        save_ppm(
                savename,
                self->frame_buffer->data(),
                self->width,
                self->height);

        // self->got_target = true;

        // GstMessage* msg = gst_message_new_application(
        //                         GST_OBJECT(self->pipeline.get()),
        //                         gst_structure_new_empty("FRAME_DONE"));
        // gst_element_post_message(self->pipeline.get(), msg);

        gst_buffer_unmap( buffer, &map );
    // }
    self->current_frame++;

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

gboolean VCU_Decode::bus_callback(GstBus* bus, GstMessage* msg, gpointer user_data)
{
    auto* loop = static_cast<GMainLoop*>( user_data );

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        {
            g_main_loop_quit(loop);
            break;
        }
    case GST_MESSAGE_APPLICATION:
        {
            g_main_loop_quit(loop);
            break;
        }
    case GST_MESSAGE_ERROR:
        {
            GError* err = nullptr;
            gchar* debug = nullptr;

            gst_message_parse_error(msg, &err, &debug);

            g_error_free(err);
            g_free(debug);
            g_main_loop_quit(loop);
            break;
        }
    default:
        break;
    }

    return GST_BUS_PASS;
}