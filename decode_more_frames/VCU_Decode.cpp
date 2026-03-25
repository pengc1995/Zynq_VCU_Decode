#include "VCU_Decode.h"
#include <Video/gst/GSTMainLoop.h>
#include <Utils/annotation_tags.h>
#include <Utils/Kron_Exception.h>
#include <fmt/format.h>
#include <iostream>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <gst/gst.h>
#pragma GCC diagnostic pop

using namespace Kron_VCU_Decode;

// ---------------------------------------------------------------------------
// decoder_init / decoder_deinit
// ---------------------------------------------------------------------------

void VCU_Decode::decoder_init(
        std::filesystem::path const& file )
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
            file_source.get(), demuxer.get(), parser.get(),
            caps_filter.get(), decoder.get(), raw_caps_filter.get(),
            app_sink.get(), nullptr );

    gst_object_ref_sink( decoding_pipeline.get() );
    gst_object_ref_sink( file_source.get() );
    gst_object_ref_sink( demuxer.get() );
    gst_object_ref_sink( parser.get() );
    gst_object_ref_sink( caps_filter.get() );
    gst_object_ref_sink( decoder.get() );
    gst_object_ref_sink( raw_caps_filter.get() );
    gst_object_ref_sink( app_sink.get() );

    gst_element_link_many( file_source.get(), demuxer.get(), nullptr );
    gst_element_link_many( parser.get(), caps_filter.get(), decoder.get(),
                           raw_caps_filter.get(), app_sink.get(), nullptr );

    // GLib main loop (bus watch)
    m_loop = g_main_loop_new( nullptr, FALSE );
    GstBus* bus = gst_element_get_bus( decoding_pipeline.get() );
    gst_bus_add_watch( bus, on_bus_callback, this );
    gst_object_unref( bus );

    m_loop_thread = std::thread( [this]() { g_main_loop_run( m_loop ); } );

    // Bring pipeline to PAUSED so it is ready to roll immediately
    gst_element_set_state( decoding_pipeline.get(), GST_STATE_PAUSED );
    gst_element_get_state( decoding_pipeline.get(), nullptr, nullptr, GST_CLOCK_TIME_NONE );

    // Start prefetch thread
    m_prefetch_running = true;
    m_prefetch_thread  = std::thread( [this]() { prefetch_loop(); } );
}

void VCU_Decode::decoder_deinit()
{
    // Stop prefetch thread
    m_prefetch_running = false;
    m_seek_cv.notify_all();
    m_queue_cv.notify_all();
    m_sample_cv.notify_all();

    if ( m_prefetch_thread.joinable() )
        m_prefetch_thread.join();

    // Stop GLib loop
    if ( m_loop ) {
        if ( g_main_loop_is_running( m_loop ) )
            g_main_loop_quit( m_loop );
        if ( m_loop_thread.joinable() )
            m_loop_thread.join();
        g_main_loop_unref( m_loop );
        m_loop = nullptr;
    }

    teardown_pipeline();
}

// ---------------------------------------------------------------------------
// decode_frame  (called by VCU_Playback_Controller)
// ---------------------------------------------------------------------------

void VCU_Decode::decode_frame(
        unsigned int  const frame_index,
        uint8_t*      const destination )
{
    // Check if this frame is already at the front of the prefetch queue
    {
        std::unique_lock<std::mutex> lock( m_queue_mutex );

        // If the requested frame is not the next sequential frame,
        // or if the queue is empty, signal a seek to the prefetch thread.
        bool const need_seek = m_decoded_queue.empty()
                            or ( m_decoded_queue.front().frame_index != frame_index
                                 and ( static_cast<int>(frame_index) < m_last_decoded_frame
                                       or static_cast<int>(frame_index) > m_last_decoded_frame + 60 ) );

        if ( need_seek ) {
            // Request seek
            {
                std::lock_guard<std::mutex> seek_lock( m_seek_mutex );
                m_seek_target    = static_cast<int>( frame_index );
                m_seek_requested = true;
            }
            m_seek_cv.notify_one();

            // Flush the queue
            m_decoded_queue.clear();
        }

        // Wait until the requested frame appears at the front of the queue
        m_queue_cv.wait( lock, [&]() {
            return !m_prefetch_running
                or ( !m_decoded_queue.empty()
                     and m_decoded_queue.front().frame_index == frame_index );
        });

        if ( !m_prefetch_running )
            return;

        // Copy from the prefetched YUV slot into the caller's destination
        Decoded_Frame const& df = m_decoded_queue.front();
        unsigned int const frame_size = meta.width * meta.height * 3u / 2u;
        memcpy( destination, df.yuv_address, frame_size );

        m_decoded_queue.pop_front();
    }

    // Wake prefetch thread — there is now a free slot
    m_queue_cv.notify_one();
}

// ---------------------------------------------------------------------------
// prefetch_loop  (runs in its own thread, pipeline always PLAYING)
// ---------------------------------------------------------------------------

void VCU_Decode::prefetch_loop()
{
    // Start pipeline
    gst_element_set_state( decoding_pipeline.get(), GST_STATE_PLAYING );
    m_pipeline_playing = true;

    unsigned int next_frame = 0u;

    while ( m_prefetch_running )
    {
        // ----------------------------------------------------------------
        // Check for a pending seek request
        // ----------------------------------------------------------------
        {
            std::unique_lock<std::mutex> seek_lock( m_seek_mutex );
            if ( m_seek_requested )
            {
                next_frame       = static_cast<unsigned int>( m_seek_target.load() );
                m_seek_requested = false;
                seek_lock.unlock();

                seek_to( next_frame );

                // Clear any stale sample signals
                {
                    std::lock_guard<std::mutex> s( m_sample_mutex );
                    m_sample_ready = false;
                    m_eos_reached  = false;
                }
            }
        }

        // ----------------------------------------------------------------
        // Wait if queue is full — no point decoding if consumer is slow
        // ----------------------------------------------------------------
        {
            std::unique_lock<std::mutex> lock( m_queue_mutex );
            m_queue_cv.wait( lock, [&]() {
                return !m_prefetch_running
                    or m_decoded_queue.size() < MAX_QUEUE_SIZE
                    or m_seek_requested.load();
            });

            if ( !m_prefetch_running )
                break;

            // Re-check seek after waking
            if ( m_seek_requested.load() )
                continue;
        }

        // ----------------------------------------------------------------
        // Allocate next YUV slot from the caller-provided allocator.
        // We reuse the yuv_buffer_controller via a function pointer / callback
        // set by VCU_Playback_Controller (see below).
        // ----------------------------------------------------------------
        if ( !m_get_yuv_slot )
        {
            // No slot allocator set yet — wait
            std::this_thread::sleep_for( std::chrono::milliseconds(1) );
            continue;
        }

        uint8_t* slot = m_get_yuv_slot();
        if ( !slot )
            continue;

        // ----------------------------------------------------------------
        // Decode one frame into the slot
        // ----------------------------------------------------------------
        bool const ok = decode_next_frame_into_slot( next_frame, slot );

        if ( !ok )
        {
            // EOS — signal queue waiters and stop prefetching until seek
            std::unique_lock<std::mutex> seek_lock( m_seek_mutex );
            m_seek_cv.wait( seek_lock, [&]() {
                return !m_prefetch_running or m_seek_requested.load();
            });
            continue;
        }

        // ----------------------------------------------------------------
        // Push decoded frame onto the queue
        // ----------------------------------------------------------------
        {
            std::lock_guard<std::mutex> lock( m_queue_mutex );
            m_decoded_queue.push_back( { next_frame, slot } );
            m_last_decoded_frame = static_cast<int>( next_frame );
        }
        m_queue_cv.notify_all();

        ++next_frame;

        // Wrap around
        if ( static_cast<int>( next_frame ) >= meta.total_frames )
            next_frame = 0u;
    }

    // Stop pipeline
    gst_element_set_state( decoding_pipeline.get(), GST_STATE_PAUSED );
    m_pipeline_playing = false;
}

// ---------------------------------------------------------------------------
// decode_next_frame_into_slot
// ---------------------------------------------------------------------------

bool VCU_Decode::decode_next_frame_into_slot(
        unsigned int frame_index,
        uint8_t*     slot_address )
{
    GstClockTime const frame_period =
        gst_util_uint64_scale( 1, GST_SECOND * meta.fps_d, meta.fps_n );

    m_current_target_pts =
        gst_util_uint64_scale( frame_index, GST_SECOND * meta.fps_d, meta.fps_n )
        + ( frame_index % 5 == 0 ? 0 : frame_period / 2 );

    m_current_target_frame = frame_index;
    m_current_slot         = slot_address;

    // Clear sample signal
    {
        std::lock_guard<std::mutex> lock( m_sample_mutex );
        m_sample_ready = false;
        m_eos_reached  = false;
    }

    // Wait for on_new_sample to deliver the frame
    std::unique_lock<std::mutex> lock( m_sample_mutex );
    m_sample_cv.wait( lock, [&]() {
        return !m_prefetch_running or m_sample_ready or m_eos_reached;
    });

    return m_sample_ready and not m_eos_reached;
}

// ---------------------------------------------------------------------------
// seek_to
// ---------------------------------------------------------------------------

void VCU_Decode::seek_to( unsigned int const frame_index )
{
    GstClockTime const frame_period =
        gst_util_uint64_scale( 1, GST_SECOND * meta.fps_d, meta.fps_n );

    GstClockTime const target_pts =
        gst_util_uint64_scale( frame_index, GST_SECOND * meta.fps_d, meta.fps_n )
        + ( frame_index % 5 == 0 ? 0 : frame_period / 2 );

    m_gop_multiple_skip =
        ( frame_index % 5 == 0 ) and ( frame_index != 0 );

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

// ---------------------------------------------------------------------------
// on_new_sample
// ---------------------------------------------------------------------------

GstFlowReturn VCU_Decode::on_new_sample(
        GstAppSink* sink,
        gpointer    user_data )
{
    auto* self = static_cast<VCU_Decode*>( user_data );
    if ( !self )
        return GST_FLOW_OK;

    GstSample* sample = gst_app_sink_pull_sample( sink );
    if ( !sample )
        return GST_FLOW_OK;

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

    if ( pts >= self->m_current_target_pts )
    {
        // GOP multiple skip logic (same as before)
        if ( self->m_gop_multiple_skip and self->m_current_target_frame != 0 )
        {
            self->m_gop_multiple_skip = false;
            gst_sample_unref( sample );
            return GST_FLOW_OK;
        }

        GstMapInfo map;
        if ( !gst_buffer_map( buffer, &map, GST_MAP_READ ) ) {
            gst_sample_unref( sample );
            return GST_FLOW_OK;
        }

        if ( self->m_current_slot )
        {
            memcpy( self->m_current_slot, map.data, map.size );
        }

        gst_buffer_unmap( buffer, &map );

        {
            std::lock_guard<std::mutex> lock( self->m_sample_mutex );
            self->m_sample_ready = true;
        }
        self->m_sample_cv.notify_one();
    }

    gst_sample_unref( sample );
    return GST_FLOW_OK;
}

// ---------------------------------------------------------------------------
// on_bus_callback
// ---------------------------------------------------------------------------

gboolean VCU_Decode::on_bus_callback(
        GstBus*     bus,
        GstMessage* msg,
        gpointer    user_data )
{
    auto* self = static_cast<VCU_Decode*>( user_data );

    switch ( GST_MESSAGE_TYPE( msg ) )
    {
    case GST_MESSAGE_EOS:
    {
        std::lock_guard<std::mutex> lock( self->m_sample_mutex );
        self->m_eos_reached = true;
        self->m_sample_cv.notify_one();
        break;
    }
    case GST_MESSAGE_ERROR:
    {
        GError* err   = nullptr;
        gchar*  debug = nullptr;
        gst_message_parse_error( msg, &err, &debug );
        std::cerr << "GStreamer error: " << (err ? err->message : "unknown") << "\n";
        g_error_free( err );
        g_free( debug );

        // Treat as EOS so prefetch_loop doesn't hang
        std::lock_guard<std::mutex> lock( self->m_sample_mutex );
        self->m_eos_reached = true;
        self->m_sample_cv.notify_one();
        break;
    }
    default:
        break;
    }

    return TRUE;
}

// ---------------------------------------------------------------------------
// on_pad_added
// ---------------------------------------------------------------------------

void VCU_Decode::on_pad_added(
        GstElement* src,
        GstPad*     pad,
        gpointer    user_data )
{
    GstElement* parser_elem = static_cast<GstElement*>( user_data );
    GstPad* sink_pad = gst_element_get_static_pad( parser_elem, "sink" );
    if ( !gst_pad_is_linked( sink_pad ) )
        gst_pad_link( pad, sink_pad );
    gst_object_unref( sink_pad );
}

// ---------------------------------------------------------------------------
// Pipeline setup (unchanged from original)
// ---------------------------------------------------------------------------

void VCU_Decode::create_pipeline( Save_File_Format const format )
{
    bool const is_h264 = ( Save_File_Format::h264 == format );

    decoding_pipeline = make_gst_pipeline( "decoding-pipeline" );
    file_source       = make_gst_element( "filesrc",                          "file-src" );
    demuxer           = make_gst_element( "qtdemux",                          "demuxer" );
    parser            = make_gst_element( is_h264 ? "h264parse" : "h265parse","parser" );
    caps_filter       = make_gst_element( "capsfilter",                       "caps-filter" );
    decoder           = make_gst_element( is_h264 ? "omxh264dec":"omxh265dec","decoder" );
    raw_caps_filter   = make_gst_element( "capsfilter",                       "raw-caps-filter" );
    app_sink          = make_gst_element( "appsink",                          "app-sink" );

    if ( !decoding_pipeline.get() ) throw Kron::Kron_Exception( MSG2USR "Unable to create pipeline." );
    if ( !decoder.get()           ) throw Kron::Kron_Exception( MSG2USR "Unable to create decoder element." );
    if ( !parser.get()            ) throw Kron::Kron_Exception( MSG2USR "Unable to create parser element." );
    if ( !file_source.get() || !demuxer.get() || !caps_filter.get()
      || !raw_caps_filter.get()   || !app_sink.get() )
        throw Kron::Kron_Exception( MSG2USR "Unable to create all elements." );
}

void VCU_Decode::teardown_pipeline()
{
    if ( decoding_pipeline.get() )
        gst_element_set_state( decoding_pipeline.get(), GST_STATE_NULL );
}

void VCU_Decode::configure_file_source( std::filesystem::path const& file )
{
    std::string filepath = file.string();
    g_object_set( G_OBJECT( file_source.get() ), "location", filepath.c_str(), nullptr );
}

void VCU_Decode::configure_demuxer()
{
    g_signal_connect( demuxer.get(), "pad-added", G_CALLBACK( on_pad_added ), parser.get() );
}

void VCU_Decode::configure_parser() {}

void VCU_Decode::configure_caps_filter( Save_File_Format const format )
{
    const char* media_type = ( Save_File_Format::h265 == format ) ? "video/x-h265" : "video/x-h264";
    g_object_set(
        G_OBJECT( caps_filter.get() ),
        "caps",
        gst_caps_new_simple( media_type,
            "alignment",     G_TYPE_STRING, "au",
            "stream-format", G_TYPE_STRING, "byte-stream",
            nullptr ),
        nullptr );
}

void VCU_Decode::configure_decoder()
{
    g_object_set( G_OBJECT( decoder.get() ), "internal-entropy-buffers", 2, nullptr );
}

void VCU_Decode::configure_raw_caps_filter()
{
    g_object_set(
        G_OBJECT( raw_caps_filter.get() ),
        "caps",
        gst_caps_new_simple( "video/x-raw", "format", G_TYPE_STRING, "NV12", nullptr ),
        nullptr );
}

void VCU_Decode::configure_app_sink()
{
    g_object_set(
        G_OBJECT( app_sink.get() ),
        "emit-signals", TRUE,
        "sync",         FALSE,
        "drop",         FALSE,   // don't drop frames — prefetch thread controls the rate
        "max-buffers",  1,
        nullptr );

    g_signal_connect( app_sink.get(), "new-sample", G_CALLBACK( on_new_sample ), this );
}

// ---------------------------------------------------------------------------
// get_video_info (unchanged)
// ---------------------------------------------------------------------------

VideoMetaData VCU_Decode::get_video_info( std::filesystem::path const& file )
{
    GError* error = nullptr;
    GstDiscoverer* discoverer = gst_discoverer_new( 5 * GST_SECOND, &error );

    if ( !discoverer )
        throw Kron::Kron_Exception( MSG2USR fmt::format( "Failed to read metadata of file {}", file.string() ) );

    std::string path_str = file.string();
    gchar* uri = gst_filename_to_uri( path_str.c_str(), nullptr );

    GstDiscovererInfo* info = gst_discoverer_discover_uri( discoverer, uri, &error );
    g_free( uri );

    if ( error ) {
        g_object_unref( discoverer );
        throw Kron::Kron_Exception( MSG2USR fmt::format( "Failed to read metadata of file {}", file.string() ) );
    }

    GList* v_streams = gst_discoverer_info_get_video_streams( info );
    if ( v_streams )
    {
        GstDiscovererVideoInfo* v_info = (GstDiscovererVideoInfo*)v_streams->data;
        meta.width   = gst_discoverer_video_info_get_width( v_info );
        meta.height  = gst_discoverer_video_info_get_height( v_info );
        meta.fps_n   = gst_discoverer_video_info_get_framerate_num( v_info );
        meta.fps_d   = gst_discoverer_video_info_get_framerate_denom( v_info );

        GstCaps* caps = gst_discoverer_stream_info_get_caps( (GstDiscovererStreamInfo*)v_info );
        std::string caps_str = gst_caps_to_string( caps );
        if      ( caps_str.find("h264") != std::string::npos ) meta.codec = Save_File_Format::h264;
        else if ( caps_str.find("h265") != std::string::npos ) meta.codec = Save_File_Format::h265;
        gst_caps_unref( caps );

        GstClockTime duration = gst_discoverer_info_get_duration( info );
        if ( duration != GST_CLOCK_TIME_NONE && meta.fps_d != 0 )
            meta.total_frames = static_cast<int>( gst_util_uint64_scale( duration, meta.fps_n, meta.fps_d * GST_SECOND ) );
    }
    else
    {
        gst_discoverer_info_unref( info );
        g_object_unref( discoverer );
        throw Kron::Kron_Exception( MSG2USR fmt::format( "Failed to open file {}", file.string() ) );
    }

    gst_discoverer_info_unref( info );
    g_object_unref( discoverer );
    return meta;
}
