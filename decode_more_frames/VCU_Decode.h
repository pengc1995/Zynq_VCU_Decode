#pragma once

#include <filesystem>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <chrono>
#include <cstdint>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#pragma GCC diagnostic pop

namespace Kron_VCU_Decode {
    enum class Save_File_Format { h264, h265 };
}

struct VideoMetaData
{
    unsigned int                      width         { 0 };
    unsigned int                      height        { 0 };
    unsigned int                      fps_n         { 0 };
    unsigned int                      fps_d         { 0 };
    int                               total_frames  { 0 };
    Kron_VCU_Decode::Save_File_Format codec         {};
};

// Wraps a GstElement with automatic unref
struct GstElementDeleter {
    void operator()( GstElement* e ) const { if (e) gst_object_unref(e); }
};
using GstElementPtr = std::unique_ptr<GstElement, GstElementDeleter>;

GstElementPtr make_gst_element( const char* factory, const char* name );
GstElementPtr make_gst_pipeline( const char* name );


class VCU_Decode
{
public:

    // -----------------------------------------------------------------------
    // YUV slot — holds both virtual and physical addresses of one YUV buffer slot
    // -----------------------------------------------------------------------

    struct YUV_Slot
    {
        uint8_t*   virt_addr { nullptr };  ///< virtual address — used for memcpy in on_new_sample
        uint64_t   phys_addr { 0 };        ///< physical address — returned to caller for convert_to_rgb
    };

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    VideoMetaData get_video_info( std::filesystem::path const& file );

    void decoder_init  ( std::filesystem::path const& file );
    void decoder_deinit();

    /// Request a specific frame. Blocks until the frame is available in the
    /// prefetch queue. Returns the physical address of the YUV slot containing
    /// the decoded frame — pass this directly to convert_to_rgb().
    /// For sequential access this returns almost instantly (already prefetched).
    /// For seeks it will take longer while the pipeline repositions.
    uint64_t decode_frame( unsigned int frame_index );

    /// Set the allocator lambda that returns the next available YUV slot
    /// (both virtual and physical addresses).
    /// Must be called after decoder_init() and before any decode_frame() calls.
    void set_yuv_slot_allocator( std::function<YUV_Slot()> allocator )
    {
        m_get_yuv_slot = std::move( allocator );
    }

private:

    // -----------------------------------------------------------------------
    // Decoded-frame queue entry
    // -----------------------------------------------------------------------

    struct Decoded_Frame
    {
        unsigned int  frame_index { 0 };
        uint8_t*      virt_addr   { nullptr };  ///< virtual address — for memcpy in on_new_sample
        uint64_t      phys_addr   { 0 };        ///< physical address — returned by decode_frame()
    };

    // -----------------------------------------------------------------------
    // Pipeline helpers
    // -----------------------------------------------------------------------

    void create_pipeline          ( Kron_VCU_Decode::Save_File_Format format );
    void teardown_pipeline        ();
    void configure_file_source    ( std::filesystem::path const& file );
    void configure_demuxer        ();
    void configure_parser         ();
    void configure_caps_filter    ( Kron_VCU_Decode::Save_File_Format format );
    void configure_decoder        ();
    void configure_raw_caps_filter();
    void configure_app_sink       ();

    // -----------------------------------------------------------------------
    // Prefetch thread
    // -----------------------------------------------------------------------

    void prefetch_loop();

    /// Decode the next sequential frame into the given slot.
    /// Blocks until on_new_sample copies the frame or EOS/shutdown occurs.
    /// Returns false on EOS or shutdown.
    bool decode_next_frame_into_slot( unsigned int frame_index, YUV_Slot const& slot );

    /// Issue a seek to bring the pipeline close to frame_index.
    void seek_to( unsigned int frame_index );

    // -----------------------------------------------------------------------
    // GStreamer callbacks (static)
    // -----------------------------------------------------------------------

    static GstFlowReturn on_new_sample  ( GstAppSink* sink, gpointer user_data );
    static void          on_pad_added   ( GstElement* src, GstPad* pad, gpointer user_data );
    static gboolean      on_bus_callback( GstBus* bus, GstMessage* msg, gpointer user_data );

    // -----------------------------------------------------------------------
    // GStreamer pipeline elements
    // -----------------------------------------------------------------------

    GstElementPtr decoding_pipeline;
    GstElementPtr file_source;
    GstElementPtr demuxer;
    GstElementPtr parser;
    GstElementPtr caps_filter;
    GstElementPtr decoder;
    GstElementPtr raw_caps_filter;
    GstElementPtr app_sink;

    GMainLoop*    m_loop        { nullptr };
    std::thread   m_loop_thread;

    // -----------------------------------------------------------------------
    // Prefetch thread state
    // -----------------------------------------------------------------------

    std::thread           m_prefetch_thread;
    std::atomic<bool>     m_prefetch_running { false };

    /// Lambda set by VCU_Playback_Controller to get the next available YUV slot.
    /// Exclusively owned by the prefetch thread — get_frame() no longer calls
    /// yuv_buffer_controller directly.
    std::function<YUV_Slot()> m_get_yuv_slot;

    /// Queue of prefetched decoded frames.
    /// Shared between prefetch thread (producer) and decode_frame() (consumer).
    std::mutex                m_queue_mutex;
    std::condition_variable   m_queue_cv;
    std::deque<Decoded_Frame> m_decoded_queue;

    static constexpr unsigned int MAX_QUEUE_SIZE { 8u };  ///< max frames to prefetch ahead

    // -----------------------------------------------------------------------
    // Seek request  (caller thread → prefetch thread)
    // -----------------------------------------------------------------------

    std::mutex              m_seek_mutex;
    std::condition_variable m_seek_cv;
    std::atomic<bool>       m_seek_requested { false };
    std::atomic<int>        m_seek_target    { -1 };   ///< -1 = no pending seek

    // -----------------------------------------------------------------------
    // on_new_sample → prefetch thread signalling
    // -----------------------------------------------------------------------

    std::mutex              m_sample_mutex;
    std::condition_variable m_sample_cv;
    bool                    m_sample_ready          { false };
    bool                    m_eos_reached           { false };

    /// Written by prefetch thread before waiting; read by on_new_sample.
    uint8_t*                m_current_slot          { nullptr };
    GstClockTime            m_current_target_pts    { GST_CLOCK_TIME_NONE };
    unsigned int            m_current_target_frame  { 0 };
    bool                    m_gop_multiple_skip     { false };

    // -----------------------------------------------------------------------
    // Misc state
    // -----------------------------------------------------------------------

    VideoMetaData           meta;
    int                     m_last_decoded_frame { -1 };  ///< last frame decoded by prefetch thread
    bool                    m_pipeline_playing   { false };
};
