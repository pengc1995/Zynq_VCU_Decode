#pragma once

#ifndef USE_HAL_MOCK

#include "Gst_object_deleter.h"
#include <gst/gstpipeline.h>
#include <gst/gstelement.h>
#include "GSTObject.h"
#include <Utils/Kron_Exception.h>
#include <fmt/format.h>


using GSTMainLoop = GSTObject<GMainLoop,decltype([]( GMainLoop *& loop )
    {
        if ( loop )
        {
            g_main_loop_unref( loop );
            loop = NULL;
        }
    })>;   ///< RAII wrapper for GMainLoop


template<
    typename...Args>
inline
GSTMainLoop
make_main_loop(
        Args&& ...args )
{
    return make_gst_object<

        GSTMainLoop,
        decltype(
            []( GMainContext *context, gboolean is_running )
            {
                return g_main_loop_new( context, is_running );
            }),
        decltype(
            []( [[maybe_unused]] GMainContext * context, [[maybe_unused]] gboolean is_running )
            {
                throw
                    Kron::Kron_Exception{
                        fmt::format(
                            MSG2USR "GST error: failed creating main loop." ) };

            })>

        (std::forward<Args>(args)...);
}

#endif
