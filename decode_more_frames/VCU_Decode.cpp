#include "VCU_Decode.h"
#include <Video/gst/GSTMainLoop.h>
#include <Utils/annotation_tags.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <gst/gst.h>
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
    gst_object_ref_sink( decoding_pipeline.get() );
    gst_object_ref_sink( file_source.get() );
    gst_object_ref_sink( demuxer.get() );
    gst_object_ref_sink( parser.get() );
    gst_object_ref_sink( caps_filter.get() );
    gst_object_ref_sink( decoder.get() );
    gst_object_ref_sink( raw_caps_filter.get() );
    gst_object_ref_sink( app_sink.get() );

    gst_element_link_many(
            file_source.get(),
            demuxer.get(),
            nullptr );

    gst_element_link_many(
            parser.get(),
            caps_filter.get(),
            decoder.get(),
            raw_caps_filter.get(),
            app_sink.get(),
            nullptr );

    m_loop = g_main_loop_new( nullptr, FALSE );

    GstBus* bus = gst_element_get_bus( decoding_pipeline.get() );
    gst_bus_add_watch( bus, on_bus_callback, this );
    gst_object_unref( bus );

    m_loop_thread = std::thread([this]() {
        g_main_loop_run(m_loop);
    });

    gst_element_set_state(
            decoding_pipeline.get(),
            GST_STATE_READY );
    gst_element_get_state(
            decoding_pipeline.get(),
            nullptr,
            nullptr,
            GST_CLOCK_TIME_NONE );
}

void VCU_Decode::decoder_deinit()
{
    stop_prefetch_thread();

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

uint64_t VCU_Decode::decode_frame(
        unsigned int const frame_index )
{
    {
        std::unique_lock<std::mutex> lock( cache_mutex );

        // Check if frame is in cache
        for ( auto const & entry : frame_cache ) {
            if ( entry.is_valid and entry.frame_index == frame_index ) {
                last_displayed_frame = frame_index;
                cache_consumed_cv.notify_one();
                return entry.yuv_slot_address;
            }
        }
    }

    // Cache miss — stop prefetch, seek to target, decode it blocking, then restart prefetch
    stop_prefetch_thread();
    flush_cache();

    uint64_t const address = decode_frame_blocking( frame_index );

    // Store decoded frame in cache slot 0
    {
        std::lock_guard<std::mutex> lock( cache_mutex );
        frame_cache[0] = CachedFrame {
            .frame_index       = frame_index,
            .yuv_slot_address  = address,
            .is_valid          = true
        };
        next_cache_slot  = 1;
        last_displayed_frame = frame_index;
    }

    start_prefetch_thread( frame_index + 1 );

    return address;
}

uint64_t VCU_Decode::decode_frame_blocking(
        unsigned int const frame_index )
{
    uint64_t const address = yuv_buffer_controller.get_the_next_address();

    uint8_t* virt_addr = reinterpret_cast<uint8_t*>(
            yuv_frame_buffer.get_virtual_address_from_physical_address( address ) );

    decode_into( frame_index, virt_addr );

    return address;
}

void VCU_Decode::decode_into(
        unsigned int  const frame_index,
        uint8_t*      const destination )
{
    virt_addr    = destination;
    target_frame = frame_index;

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        got_target = false;
    }

    GstClockTime const frame_period = gst_util_uint64_scale( 1, GST_SECOND * meta.fps_d, meta.fps_n );
    target_pts = gst_util_uint64_scale( target_frame, GST_SECOND * meta.fps_d, meta.fps_n )
               + ( target_frame % 5 == 0 ? 0 : frame_period / 2 );

    bool const is_seek = ( target_frame < last_gst_frame )
                      or ( target_frame > last_gst_frame + 1 );

    gop_multiple_skip = ( is_seek and ( target_frame % 5 == 0 ) );

    if ( is_seek ) {
        got_target = true;
        gst_element_set_state(
            decoding_pipeline.get(),
            GST_STATE_PLAYING );
        got_target = false;

        gst_element_seek(
            decoding_pipeline.get(),
            1.0,
            GST_FORMAT_TIME,
            static_cast<GstSeekFlags>( GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT ),
            GST_SEEK_TYPE_SET,
            target_pts,
            GST_SEEK_TYPE_NONE,
            GST_CLOCK_TIME_NONE );
    }
    else {
        gst_element_set_state(
            decoding_pipeline.get(),
            GST_STATE_PLAYING );
    }

    {
        std::unique_lock<std::mutex> lock( m_mutex );
        m_cv.wait( lock, [this]() {
            return got_target;
        });
    }

    gst_element_set_state(
            decoding_pipeline.get(),
            GST_STATE_PAUSED );

    last_gst_frame = frame_index;
}

void VCU_Decode::start_prefetch_thread(
        unsigned int const start_frame )
{
    prefetch_stop_flag = false;
    prefetch_next_frame = start_frame;

    prefetch_thread = std::thread( [this]() {
        prefetch_loop();
    });
}

void VCU_Decode::stop_prefetch_thread()
{
    {
        std::lock_guard<std::mutex> lock( cache_mutex );
        prefetch_stop_flag = true;
    }
    cache_consumed_cv.notify_all();

    if ( prefetch_thread.joinable() ) {
        prefetch_thread.join();
    }
}

void VCU_Decode::flush_cache()
{
    std::lock_guard<std::mutex> lock( cache_mutex );
    for ( auto & entry : frame_cache ) {
        entry.is_valid = false;
    }
    next_cache_slot = 0;
}

void VCU_Decode::prefetch_loop()
{
    while ( true ) {
        unsigned int frame_to_decode = 0;
        uint64_t     slot_address    = 0;
        int          slot_index      = 0;

        {
            std::unique_lock<std::mutex> lock( cache_mutex );

            // Wait until there is a free slot or stop is requested
            cache_consumed_cv.wait( lock, [this]() {
                if ( prefetch_stop_flag ) return true;
                int valid_count = 0;
                for ( auto const & e : frame_cache ) {
                    if ( e.is_valid ) valid_count++;
                }
                return valid_count < PREFETCH_CACHE_SIZE;
            });

            if ( prefetch_stop_flag ) return;

            frame_to_decode = prefetch_next_frame;
            slot_index      = next_cache_slot % PREFETCH_CACHE_SIZE;
            slot_address    = yuv_buffer_controller.get_the_next_address();

            // Reserve the slot
            frame_cache[slot_index] = CachedFrame {
                .frame_index      = frame_to_decode,
                .yuv_slot_address = slot_address,
                .is_valid         = false   // not valid yet, being decoded
            };
        }

        // Decode outside the lock so get_frame() is not blocked during decode
        uint8_t* virt_addr = reinterpret_cast<uint8_t*>(
                yuv_frame_buffer.get_virtual_address_from_physical_address( slot_address ) );

        decode_into( frame_to_decode, virt_addr );

        {
            std::lock_guard<std::mutex> lock( cache_mutex );

            if ( prefetch_stop_flag ) return;

            frame_cache[slot_index].is_valid = true;
            prefetch_next_frame++;
            next_cache_slot = ( next_cache_slot + 1 ) % PREFETCH_CACHE_SIZE;
        }
    }
}

VideoMetaData VCU_Decode::get_video_info(
        std::filesystem::path const & file )
{
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
        if ( caps_str.find( "h264" ) != std::string::npos ) {
            meta.codec = Save_File_Format::h264;
        }
        else if ( caps_str.find( "h265" ) != std::string::npos ) {
            meta.codec = Save_File_Format::h265;
        }
        gst_caps_unref( caps );

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

    return meta;
}

void VCU_Decode::create_pipeline(
        Save_File_Format const format )
{
    bool const is_h264{ Save_File_Format::h264 == format };

    decoding_pipeline = make_gst_pipeline( "decoding-pipeline" );

    file_source       = make_gst_element( "filesrc",                              "file-src"       );
    demuxer           = make_gst_element( "qtdemux",                              "demuxer"        );
    parser            = make_gst_element( is_h264 ? "h264parse" : "h265parse",    "parser"         );
    caps_filter       = make_gst_element( "capsfilter",                           "caps-filter"    );
    decoder           = make_gst_element( is_h264 ? "omxh264dec" : "omxh265dec",  "decoder"        );
    raw_caps_filter   = make_gst_element( "capsfilter",                           "raw-caps-filter");
    app_sink          = make_gst_element( "appsink",                              "app-sink"       );

    if ( !decoding_pipeline.get() ) {
        throw Kron::Kron_Exception( MSG2USR "Unable to create pipeline." );
    }
    else if ( !decoder.get() ) {
        throw Kron::Kron_Exception( MSG2USR "Unable to create decoder element." );
    }
    else if ( !parser.get() ) {
        throw Kron::Kron_Exception( MSG2USR "Unable to create parser element." );
    }
    else if ( !file_source.get()
    ||        !demuxer.get()
    ||        !caps_filter.get()
    ||        !raw_caps_filter.get()
    ||        !app_sink.get() ) {
        throw Kron::Kron_Exception( MSG2USR "Unable to create all elements." );
    }
}

void VCU_Decode::teardown_pipeline()
{
    if ( decoding_pipeline.get() ) {
        gst_element_set_state(
            decoding_pipeline.get(),
            GST_STATE_NULL );
    }
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
    if ( Save_File_Format::h265 == format ) {
        g_object_set(
                G_OBJECT( caps_filter.get() ),
                "caps",
                gst_caps_new_simple(
                    "video/x-h265",
                    "alignment",     G_TYPE_STRING, "au",
                    "stream-format", G_TYPE_STRING, "byte-stream",
                    nullptr ),
                nullptr );
    }
    else {
        g_object_set(
                G_OBJECT( caps_filter.get() ),
                "caps",
                gst_caps_new_simple(
                    "video/x-h264",
                    "alignment",     G_TYPE_STRING, "au",
                    "stream-format", G_TYPE_STRING, "byte-stream",
                    nullptr ),
                nullptr );
    }
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

    if ( self->got_target ) {
        gst_sample_unref( sample );
        return GST_FLOW_OK;
    }

    GstBuffer* buffer = gst_sample_get_buffer( sample );
    if ( !buffer ) {
        gst_sample_unref( sample );
        return GST_FLOW_OK;
    }

    GstClockTime pts = GST_BUFFER_PTS( buffer );
    if ( !GST_CLOCK_TIME_IS_VALID( pts ) ) {
        gst_sample_unref( sample );
        return GST_FLOW_OK;
    }

    if ( pts >= self->target_pts ) {
        // When using seek() and target_frame is a multiple of 5 (not including 0),
        // the first pts matching the condition needs to be skipped
        if ( self->gop_multiple_skip and self->target_frame != 0 ) {
            self->gop_multiple_skip = false;
            return GST_FLOW_OK;
        }

        GstMapInfo map;
        if ( !gst_buffer_map( buffer, &map, GST_MAP_READ ) ) {
            gst_sample_unref( sample );
            return GST_FLOW_OK;
        }

        memcpy(
                self->virt_addr,
                map.data,
                map.size );

        {
            std::lock_guard<std::mutex> lock( self->m_mutex );
            self->got_target = true;
        }
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
        {
            std::lock_guard<std::mutex> lock( self->m_mutex );
            self->got_target = true;
        }
        self->m_cv.notify_one();
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
