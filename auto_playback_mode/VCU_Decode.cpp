#include "VCU_Decode.h"
#include <Utils/Kron_Exception.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/pbutils/pbutils.h>
#pragma GCC diagnostic pop

using namespace Kron_VCU_Decode;

void VCU_Decode::decoder_init(
        std::filesystem::path const & file )
{
    create_pipeline( meta.codec );

    configure_file_source( file );
    configure_demuxer();
    configure_parser();
    configure_caps_filter( meta.codec );
    configure_decoder();
    configure_raw_caps_filter();
    configure_app_sink();

    gst_bin_add_many(
            GST_BIN( decoding_pipeline.get() ),
            file_source.get(),
            demuxer.get(),
            parser.get(),
            caps_filter.get(),
            decoder.get(),
            raw_caps_filter.get(),
            app_sink.get(),
            nullptr );

    gst_element_link_many( file_source.get(), demuxer.get(), nullptr );
    gst_element_link_many( parser.get(), caps_filter.get(), decoder.get(), raw_caps_filter.get(), app_sink.get(), nullptr );

    m_loop = g_main_loop_new( nullptr, FALSE );
    GstBus* bus = gst_element_get_bus( decoding_pipeline.get() );
    gst_bus_add_watch( bus, on_bus_callback, this );
    gst_object_unref( bus );

    m_loop_thread = std::thread([this]() {
        g_main_loop_run(m_loop);
    });

    gst_element_set_state( decoding_pipeline.get(), GST_STATE_READY );
    gst_element_get_state( decoding_pipeline.get(), nullptr, nullptr, GST_CLOCK_TIME_NONE );
}

void VCU_Decode::decoder_deinit()
{
    stop_continuous_playback();

    if ( m_loop ) {
        if ( g_main_loop_is_running( m_loop ) ) {
            g_main_loop_quit( m_loop );
        }
        if ( m_loop_thread.joinable() ) {
            m_loop_thread.join();
        }
        g_main_loop_unref( m_loop );
        m_loop = nullptr;
    }

    teardown_pipeline();
}

void VCU_Decode::decode_frame(
        unsigned int  const frame_index,
        uint8_t*      const destination )
{
    virt_addr  = destination;
    got_target = false;

    GstClockTime const frame_period = gst_util_uint64_scale( 1, GST_SECOND * meta.fps_d, meta.fps_n );
    target_pts = gst_util_uint64_scale( frame_index, GST_SECOND * meta.fps_d, meta.fps_n )
               + ( frame_index % meta.gop_size == 0 ? 0 : frame_period / 2 );

    bool const is_jump = ( frame_index < last_frame ) or ( frame_index > last_frame + meta.gop_size );

    gop_multiple_skip = meta.has_b_frames
                    and ( frame_index % meta.gop_size == 0 )
                    and ( frame_index != 0 )
                    and ( is_jump or frame_index > last_frame + 1 );

    if ( is_jump ) {
        // Temporarily block on_new_sample during PAUSED -> PLAYING transition
        // before seek is issued
        got_target = true;
        gst_element_set_state( decoding_pipeline.get(), GST_STATE_PLAYING );

        gst_element_seek(
            decoding_pipeline.get(),
            1.0,
            GST_FORMAT_TIME,
            static_cast<GstSeekFlags>( GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT ),
            GST_SEEK_TYPE_SET, target_pts,
            GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE );

        got_target = false;
    }
    else {
        gst_element_set_state( decoding_pipeline.get(), GST_STATE_PLAYING );
    }

    {
        std::unique_lock<std::mutex> lock( m_mutex );
        m_cv.wait( lock, [this]() {
            return got_target.load();
        });
    }

    gst_element_set_state( decoding_pipeline.get(), GST_STATE_PAUSED );
    last_frame = frame_index;
}

void VCU_Decode::start_continuous_playback(
        double    const rate,
        uint64_t  const initial_yuv_address,
        UDMABuf                          & yuv_frame_buffer_,
        Circular_Frame_Buffer_Controller & yuv_buffer_controller_ )
{
    yuv_frame_buffer      = &yuv_frame_buffer_;
    yuv_buffer_controller = &yuv_buffer_controller_;

    // Initialise front buffer with last decoded frame from decode_frame()
    // so get_latest_yuv_address() is always valid from the start
    front_address = initial_yuv_address;

    // Get first back buffer slot ready for on_new_sample
    back_address = yuv_buffer_controller->get_the_next_address();

    is_continuous_playback = true;

    // Set rate and start PLAYING
    gst_element_set_state( decoding_pipeline.get(), GST_STATE_PLAYING );

    gint64 position = 0;
    gst_element_query_position( decoding_pipeline.get(), GST_FORMAT_TIME, &position );

    gst_element_seek(
        decoding_pipeline.get(),
        rate,
        GST_FORMAT_TIME,
        GST_SEEK_FLAG_FLUSH,
        GST_SEEK_TYPE_SET,
        position,
        GST_SEEK_TYPE_NONE,
        GST_CLOCK_TIME_NONE );
}

void VCU_Decode::stop_continuous_playback()
{
    if ( !is_continuous_playback ) return;

    is_continuous_playback = false;

    gst_element_set_state( decoding_pipeline.get(), GST_STATE_PAUSED );

    yuv_frame_buffer      = nullptr;
    yuv_buffer_controller = nullptr;
}

void VCU_Decode::set_continuous_playback_rate(
        double const rate )
{
    gint64 position = 0;
    gst_element_query_position( decoding_pipeline.get(), GST_FORMAT_TIME, &position );

    gst_element_seek(
        decoding_pipeline.get(),
        rate,
        GST_FORMAT_TIME,
        GST_SEEK_FLAG_FLUSH,
        GST_SEEK_TYPE_SET,
        position,
        GST_SEEK_TYPE_NONE,
        GST_CLOCK_TIME_NONE );
}

uint64_t VCU_Decode::get_latest_yuv_address() const noexcept
{
    return front_address.load();
}

VideoMetaData VCU_Decode::get_video_info(
        std::filesystem::path const & file )
{
    // -------------------------------------------------------------------------
    // GstDiscoverer — width, height, fps, codec, total_frames
    // -------------------------------------------------------------------------
    GError* error = nullptr;
    GstDiscoverer* discoverer = gst_discoverer_new( 5 * GST_SECOND, &error );

    if ( !discoverer ) {
        throw Kron::Kron_Exception(
                MSG2USR fmt::format( "Failed to read metadata of file {}", file.string() ) );
    }

    std::string path_str = file.string();
    gchar* uri = gst_filename_to_uri( path_str.c_str(), nullptr );

    GstDiscovererInfo* info = gst_discoverer_discover_uri( discoverer, uri, &error );
    g_free( uri );

    if ( error ) {
        g_error_free( error );
        g_object_unref( discoverer );
        throw Kron::Kron_Exception(
                MSG2USR fmt::format( "Failed to read metadata of file {}", file.string() ) );
    }

    GList* v_streams = gst_discoverer_info_get_video_streams( info );
    if ( v_streams ) {
        GstDiscovererVideoInfo* v_info = (GstDiscovererVideoInfo*)v_streams->data;

        meta.width  = gst_discoverer_video_info_get_width( v_info );
        meta.height = gst_discoverer_video_info_get_height( v_info );
        meta.fps_n  = gst_discoverer_video_info_get_framerate_num( v_info );
        meta.fps_d  = gst_discoverer_video_info_get_framerate_denom( v_info );

        GstCaps* caps = gst_discoverer_stream_info_get_caps( (GstDiscovererStreamInfo*)v_info );
        std::string caps_str = gst_caps_to_string( caps );
        if ( caps_str.contains( "h264" ) ) {
            meta.codec = Save_File_Format::h264;
        }
        else if ( caps_str.contains( "h265" ) ) {
            meta.codec = Save_File_Format::h265;
        }
        else {
            gst_caps_unref( caps );
            gst_discoverer_stream_info_list_free( v_streams );
            gst_discoverer_info_unref( info );
            g_object_unref( discoverer );
            throw Kron::Kron_Exception(
                MSG2USR fmt::format( "Unsupported format. Playback supports H.264 and H.265 files." ) );
        }
        gst_caps_unref( caps );
        gst_discoverer_stream_info_list_free( v_streams );

        GstClockTime duration = gst_discoverer_info_get_duration( info );
        if ( duration != GST_CLOCK_TIME_NONE && meta.fps_d != 0 ) {
            meta.total_frames = static_cast<int>( gst_util_uint64_scale( duration, meta.fps_n, meta.fps_d * GST_SECOND ) );
        }
    }
    else {
        gst_discoverer_info_unref( info );
        g_object_unref( discoverer );
        throw Kron::Kron_Exception(
                MSG2USR fmt::format( "Failed to open file {}", file.string() ) );
    }

    gst_discoverer_info_unref( info );
    g_object_unref( discoverer );

    // -------------------------------------------------------------------------
    // ffprobe — detect B-frames and GOP size from actual frame types.
    //
    // Reads only the first 60 frames to keep it fast.
    // Example output for b-frames=4: I B B B B P B B B B P ...
    // Example output for b-frames=0: I P P P P P P P P P P ...
    // -------------------------------------------------------------------------
    meta.has_b_frames = false;
    meta.gop_size     = 1;

    std::string const cmd = fmt::format(
        "ffprobe -v quiet -select_streams v:0"
        " -show_entries frame=pict_type"
        " -of csv=p=0"
        " -read_intervals \"%+#60\""
        " \"{}\"",
        file.string() );

    FILE* pipe = popen( cmd.c_str(), "r" );
    if ( pipe ) {
        std::string output;
        char        buf[256];
        while ( fgets( buf, sizeof(buf), pipe ) ) {
            output += buf;
        }
        pclose( pipe );

        meta.has_b_frames = ( output.find('B') != std::string::npos );

        if ( meta.has_b_frames ) {
            // GOP size = distance between consecutive keyframes.
            // For IBBBBP pattern, first 'P' appears at index 5 -> gop_size = 5.
            unsigned int frame_count     = 0;
            unsigned int gop_size        = 0;
            bool         found_first_key = false;

            for ( char const c : output ) {
                if ( c != 'I' and c != 'P' and c != 'B' ) continue;

                if ( !found_first_key and ( c == 'I' or c == 'P' ) ) {
                    found_first_key = true;
                    frame_count = 0;
                }
                else if ( found_first_key ) {
                    frame_count++;
                    if ( c == 'I' or c == 'P' ) {
                        gop_size = frame_count;
                        break;
                    }
                }
            }

            if ( gop_size > 0 ) {
                meta.gop_size = gop_size;
            }
        }
    }
    else {
        spdlog::warn( "VCU_Decode: ffprobe failed for {}, assuming no B-frames.", file.string() );
    }

    spdlog::info( "VCU_Decode: {} — has_b_frames={}, gop_size={}",
                  file.string(), meta.has_b_frames, meta.gop_size );

    return meta;
}

void VCU_Decode::create_pipeline(
        Save_File_Format const format )
{
    bool const is_h264{ Save_File_Format::h264 == format };

    decoding_pipeline = make_gst_pipeline( "decoding-pipeline" );
    file_source       = make_gst_element( "filesrc",                             "file-src" );
    demuxer           = make_gst_element( "qtdemux",                             "demuxer" );
    parser            = make_gst_element( is_h264 ? "h264parse" : "h265parse",   "parser" );
    caps_filter       = make_gst_element( "capsfilter",                          "caps-filter" );
    decoder           = make_gst_element( is_h264 ? "omxh264dec" : "omxh265dec", "decoder" );
    raw_caps_filter   = make_gst_element( "capsfilter",                          "raw-caps-filter" );
    app_sink          = make_gst_element( "appsink",                             "app-sink" );
}

void VCU_Decode::teardown_pipeline()
{
    if ( decoding_pipeline.get() ) {
        gst_element_set_state(
            decoding_pipeline.get(),
            GST_STATE_NULL );
    }
    gst_object_unref( GST_PIPELINE( decoding_pipeline.get() ) );

    file_source.clear();
    demuxer.clear();
    parser.clear();
    caps_filter.clear();
    decoder.clear();
    raw_caps_filter.clear();
    app_sink.clear();
    decoding_pipeline.clear();
}

void VCU_Decode::configure_file_source(
        std::filesystem::path const & file )
{
    std::string filepath = file.string();
    g_object_set(
            G_OBJECT( file_source.get() ),
            "location", filepath.c_str(),
            nullptr );
}

void VCU_Decode::configure_demuxer()
{
    g_signal_connect(
            demuxer.get(),
            "pad-added", G_CALLBACK( on_pad_added ),
            parser.get() );
}

void VCU_Decode::configure_parser()
{}

void VCU_Decode::configure_caps_filter(
        Save_File_Format const format )
{
    const char* media_type = ( Save_File_Format::h264 == format ) ? "video/x-h264" : "video/x-h265";
    g_object_set(
            G_OBJECT( caps_filter.get() ),
            "caps",
            gst_caps_new_simple(
                media_type,
                "alignment",     G_TYPE_STRING, "au",
                "stream-format", G_TYPE_STRING, "byte-stream",
                nullptr ),
            nullptr );
}

void VCU_Decode::configure_decoder()
{
    g_object_set(
        G_OBJECT( decoder.get() ),
        "internal-entropy-buffers", 2,
        nullptr );
}

void VCU_Decode::configure_raw_caps_filter()
{
    g_object_set(
            G_OBJECT( raw_caps_filter.get() ),
            "caps",
            gst_caps_new_simple(
                "video/x-raw",
                "format", G_TYPE_STRING, "NV12",
                nullptr ),
            nullptr );
}

void VCU_Decode::configure_app_sink()
{
    g_object_set(
            G_OBJECT( app_sink.get() ),
            "emit-signals", TRUE,
            "sync",         FALSE,
            "drop",         TRUE,
            "max-buffers",  1,
            nullptr );

    g_signal_connect(
            app_sink.get(),
            "new-sample", G_CALLBACK( on_new_sample ),
            this );
}

GstFlowReturn VCU_Decode::on_new_sample(
        GstAppSink* sink,
        gpointer    user_data )
{
    auto* self = static_cast<VCU_Decode*>( user_data );
    if ( !self ) {
        return GST_FLOW_OK;
    }

    GstSample* sample = gst_app_sink_pull_sample( sink );
    if ( !sample ) {
        return GST_FLOW_OK;
    }

    GstBuffer* buffer = gst_sample_get_buffer( sample );
    if ( !buffer ) {
        gst_sample_unref( sample );
        return GST_FLOW_OK;
    }

    // -------------------------------------------------------------------------
    // Continuous playback mode — take every frame, swap double buffer
    // -------------------------------------------------------------------------
    if ( self->is_continuous_playback ) {
        GstMapInfo map;
        if ( !gst_buffer_map( buffer, &map, GST_MAP_READ ) ) {
            gst_sample_unref( sample );
            return GST_FLOW_OK;
        }

        uint8_t* vaddr = reinterpret_cast<uint8_t*>(
            self->yuv_frame_buffer->get_virtual_address_from_physical_address( self->back_address ) );

        memcpy( vaddr, map.data, map.size );

        // Swap: back becomes front atomically, advance back to next slot
        self->front_address.store( self->back_address );
        self->back_address = self->yuv_buffer_controller->get_the_next_address();

        gst_buffer_unmap( buffer, &map );
        gst_sample_unref( sample );
        return GST_FLOW_OK;
    }

    // -------------------------------------------------------------------------
    // Seek/position mode — PTS matching to find target frame
    // -------------------------------------------------------------------------
    if ( self->got_target ) {
        gst_sample_unref( sample );
        return GST_FLOW_OK;
    }

    GstClockTime const pts = GST_BUFFER_PTS( buffer );
    if ( !GST_CLOCK_TIME_IS_VALID( pts ) ) {
        gst_sample_unref( sample );
        return GST_FLOW_OK;
    }

    if ( pts >= self->target_pts ) {
        if ( self->gop_multiple_skip ) {
            self->gop_multiple_skip = false;
            gst_sample_unref( sample );
            return GST_FLOW_OK;
        }

        GstMapInfo map;
        if ( !gst_buffer_map( buffer, &map, GST_MAP_READ ) ) {
            gst_sample_unref( sample );
            return GST_FLOW_OK;
        }

        memcpy( self->virt_addr, map.data, map.size );

        self->got_target = true;
        self->m_cv.notify_one();

        gst_buffer_unmap( buffer, &map );
    }

    gst_sample_unref( sample );
    return GST_FLOW_OK;
}

void VCU_Decode::on_pad_added(
        GstElement* src,
        GstPad*     pad,
        gpointer    user_data )
{
    GstElement* parser_elem = static_cast<GstElement*>( user_data );
    GstPad* sink_pad = gst_element_get_static_pad( parser_elem, "sink" );

    if ( !gst_pad_is_linked( sink_pad ) ) {
        gst_pad_link( pad, sink_pad );
    }
    gst_object_unref( sink_pad );
}

gboolean VCU_Decode::on_bus_callback(
        GstBus*     bus,
        GstMessage* msg,
        gpointer    user_data )
{
    auto* self = static_cast<VCU_Decode*>( user_data );

    switch ( GST_MESSAGE_TYPE( msg ) ) {
    case GST_MESSAGE_EOS:
    {
        if ( !self->is_continuous_playback ) {
            self->got_target = true;
            self->m_cv.notify_one();
        }
        // TODO: handle EOS during continuous playback (loop or stop)
        break;
    }
    case GST_MESSAGE_ERROR:
    {
        GError* err   = nullptr;
        gchar*  debug = nullptr;

        gst_message_parse_error( msg, &err, &debug );

        g_error_free( err );
        g_free( debug );
        break;
    }
    default:
        break;
    }

    return TRUE;
}
