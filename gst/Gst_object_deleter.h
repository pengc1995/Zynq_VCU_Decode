#pragma once

#ifndef USE_HAL_MOCK

#include <gst/gstelement.h>

inline auto const gst_object_deleter = []( GstElement *& element )
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
        if ( element )
        {
            gst_object_unref( element );
            element = NULL;
        }
#pragma GCC diagnostic pop
    };

#endif
