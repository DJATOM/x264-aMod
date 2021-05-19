/*****************************************************************************
 * vpy.c: VapourSynth input
 *****************************************************************************
 * Copyright (C) 2009-2021 x264 project
 *
 * Author: Vladimir Kontserenko <djatom@beatrice-raws.org>
 * Some portions of code and ideas taken from avs.c, y4m.c files, "ffmpeg demuxer"
 * proposed at doom9 thread and from rigaya's NVEnc codebase.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@x264.com.
 *****************************************************************************/

#include "input.h"

#ifdef _MSC_VER
typedef intptr_t atomic_int;
#define atomic_load(object) _InterlockedCompareExchange64(object, 0, 0)
#define atomic_fetch_add(object, operand) _InterlockedExchangeAdd64(object, operand)
#define atomic_fetch_sub(object, operand) _InterlockedExchangeAdd64(object, -(operand))
#else
#include <stdatomic.h>
#endif
#include "extras/VSScript.h"
#include "extras/VSHelper.h"

#ifdef _M_IX86
#define VPY_X64 0
#else
#define VPY_X64 1
#endif

#ifdef _WIN32
typedef WCHAR libp_t;
#define vs_open(library) LoadLibraryW( (LPWSTR)library )
#define vs_close FreeLibrary
#define vs_address GetProcAddress
#define vs_sleep() Sleep(500)
#define vs_strtok strtok_s
#define vs_sscanf sscanf_s
#define CloseEvent CloseHandle
#else
typedef char libp_t;
#include <dlfcn.h>
#include <unistd.h>
#include <ctype.h>
#define avs_open(library) dlopen( library, RTLD_GLOBAL | RTLD_LAZY | RTLD_NOW )
#define avs_close dlclose
#define avs_address dlsym
#define vs_sleep() usleep(500)
#define vs_strtok strtok_r
#define vs_sscanf sscanf
#endif

#define DECLARE_VS_FUNC(name) func_##name name

#define LOAD_VS_FUNC(name, namex86)\
{\
    h->func.name = (void*)vs_address( h->library, (VPY_X64) ? #name : namex86 );\
    if( !h->func.name )\
        goto fail;\
}

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "vpy", __VA_ARGS__ )

typedef int (VS_CC *func_vsscript_init)(void);
typedef int (VS_CC *func_vsscript_finalize)(void);
typedef int (VS_CC *func_vsscript_evaluateFile)(VSScript **handle, const char *scriptFilename, int flags);
typedef void (VS_CC *func_vsscript_freeScript)(VSScript *handle);
typedef const char * (VS_CC *func_vsscript_getError)(VSScript *handle);
typedef VSNodeRef * (VS_CC *func_vsscript_getOutput)(VSScript *handle, int index);
typedef VSCore * (VS_CC *func_vsscript_getCore)(VSScript *handle);
typedef const VSAPI * (VS_CC *func_vsscript_getVSApi2)(int version);

typedef struct VapourSynthContext {
    void *library;
    const VSAPI *vsapi;
    VSScript *script;
    VSNodeRef *node;
    atomic_int async_requested;
    atomic_int async_completed;
    atomic_int async_consumed;
    atomic_int async_pending;
    HANDLE *async_frame_done_event;
    int async_requests;
    int async_start_frame;
    const VSFrameRef **async_buffer;
    int async_failed_frame;
    int num_frames;
    int bit_depth;
    int uc_depth;
    int vfr;
    uint64_t timebase_num;
    uint64_t timebase_den;
    int64_t current_timecode_num;
    int64_t current_timecode_den;
    struct
    {
        DECLARE_VS_FUNC( vsscript_init );
        DECLARE_VS_FUNC( vsscript_finalize );
        DECLARE_VS_FUNC( vsscript_evaluateFile );
        DECLARE_VS_FUNC( vsscript_freeScript );
        DECLARE_VS_FUNC( vsscript_getError );
        DECLARE_VS_FUNC( vsscript_getOutput );
        DECLARE_VS_FUNC( vsscript_getCore );
        DECLARE_VS_FUNC( vsscript_getVSApi2 );
    } func;
} VapourSynthContext;

static int custom_vs_load_library( VapourSynthContext *h, cli_input_opt_t *opt )
{
#ifdef _WIN32
    libp_t *library_path= L"vsscript";
    libp_t *tmp_buf;
#else
#if SYS_MACOSX
    libp_t *library_path = "libvapoursynth-script.dylib";
#else
    libp_t *library_path = "libvapoursynth-script.so";
#endif
#endif
    if( opt->frameserver_lib_path )
    {
#ifdef _WIN32
        int size_needed = MultiByteToWideChar( CP_UTF8, 0, opt->frameserver_lib_path, -1, NULL, 0 );
        tmp_buf = malloc( size_needed * sizeof(libp_t) );
        MultiByteToWideChar( CP_UTF8, 0, opt->frameserver_lib_path, -1, (LPWSTR)tmp_buf, size_needed );
        library_path = tmp_buf;
#else
        library_path = opt->frameserver_lib_path;
#endif
        x264_cli_log( "vpy", X264_LOG_INFO, "using external Vapoursynth library from %s\n", opt->frameserver_lib_path );
    }
    h->library = vs_open( library_path );
#ifdef _WIN32
    if( opt->frameserver_lib_path )
    {
        free( tmp_buf );
    }
#endif
    if( !h->library )
        return -1;
    LOAD_VS_FUNC( vsscript_init, "_vsscript_init@0" );
    LOAD_VS_FUNC( vsscript_finalize, "_vsscript_finalize@0" );
    LOAD_VS_FUNC( vsscript_evaluateFile, "_vsscript_evaluateFile@12" );
    LOAD_VS_FUNC( vsscript_freeScript, "_vsscript_freeScript@4" );
    LOAD_VS_FUNC( vsscript_getError, "_vsscript_getError@4" );
    LOAD_VS_FUNC( vsscript_getOutput, "_vsscript_getOutput@8" );
    LOAD_VS_FUNC( vsscript_getCore, "_vsscript_getCore@4" );
    LOAD_VS_FUNC( vsscript_getVSApi2, "_vsscript_getVSApi2@4" );
    return 0;
fail:
    vs_close( h->library );
    h->library = NULL;
    return -1;
}

static void VS_CC async_callback( void *user_data, const VSFrameRef *f, int n, VSNodeRef *node, const char *error_msg )
{
    VapourSynthContext *h = user_data;

    if ( !f ) {
        h->async_failed_frame = n;
        x264_cli_log( "vpy", X264_LOG_ERROR, "async frame request #%d failed: %s\n", n, error_msg );
    } else
        h->async_buffer[n] = f;

    atomic_fetch_sub( &h->async_pending, 1 );
    SetEvent( h->async_frame_done_event[n] );
}

/* Slightly modified rigaya's VersionString parser. */
int get_core_revision( const char *vsVersionString )
{
    char *api_info = NULL;
    char buf[1024];
    strcpy( buf, vsVersionString );
    for ( char *p = buf, *q = NULL, *r = NULL; NULL != ( q = vs_strtok( p, "\n", &r ) ); ) {
        if ( NULL != ( api_info = strstr( q, "Core" ) ) ) {
            strcpy( buf, api_info );
            for ( char *s = buf; *s; s++ )
                *s = (char)tolower( *s );
            int rev = 0;
            return ( 1 == vs_sscanf( buf, "core r%d", &rev ) ) ? rev : 0;
        }
        p = NULL;
    }
    return 0;
}

static int open_file( char *psz_filename, hnd_t *p_handle, video_info_t *info, cli_input_opt_t *opt )
{
    FILE *fh = x264_fopen( psz_filename, "r" );
    if( !fh )
        return -1;
    int b_regular = x264_is_regular_file( fh );
    fclose( fh );
    FAIL_IF_ERROR( !b_regular, "vpy input is incompatible with non-regular file `%s'\n", psz_filename );

    VapourSynthContext *h = calloc( 1, sizeof(VapourSynthContext) );
    if( !h )
        return -1;
    FAIL_IF_ERROR( custom_vs_load_library( h, opt ), "failed to load VapourSynth\n" );
    if( !h->func.vsscript_init() )
        FAIL_IF_ERROR( 1, "failed to initialize VapourSynth environment\n" );
    h->vsapi = h->func.vsscript_getVSApi2( VAPOURSYNTH_API_VERSION );
    FAIL_IF_ERROR( !h->vsapi, "failed to get vapoursynth API\n" );
    if( h->func.vsscript_evaluateFile( &h->script, (const char *)psz_filename, efSetWorkingDir ) )
        FAIL_IF_ERROR( 1, "Can't evaluate script: %s\n",  h->func.vsscript_getError( h->script ) );
    h->node = h->func.vsscript_getOutput( h->script, 0 );
    FAIL_IF_ERROR( !h->node, "`%s' has no video data\n", psz_filename );

    const VSCoreInfo *core_info = h->vsapi->getCoreInfo( h->func.vsscript_getCore( h->script ) );
    const VSVideoInfo *vi = h->vsapi->getVideoInfo( h->node );
    FAIL_IF_ERROR( !isConstantFormat(vi), "only constant video formats are supported\n" );
    x264_cli_log( "vpy", X264_LOG_INFO, "VapourSynth Video Processing Library Core R%d\n", get_core_revision( core_info->versionString ) );
    info->width = vi->width;
    info->height = vi->height;
    info->vfr = h->vfr = 0;

    h->async_start_frame = 0;
    h->async_completed = 0;
    h->async_failed_frame = -1;
    if( opt->seek > 0 )
    {
        h->async_start_frame = opt->seek;
        h->async_completed = opt->seek;
    }

    char errbuf[256];
    const VSFrameRef *frame0 = NULL;
    frame0 = h->vsapi->getFrame( h->async_completed, h->node, errbuf, sizeof(errbuf) );
    FAIL_IF_ERROR( !frame0, "%s occurred while getting frame %d\n", h->async_completed, errbuf );
    const VSMap *props = h->vsapi->getFramePropsRO( frame0 );
    int err_sar_num, err_sar_den;
    int64_t sar_num = h->vsapi->propGetInt( props, "_SARNum", 0, &err_sar_num );
    int64_t sar_den = h->vsapi->propGetInt( props, "_SARDen", 0, &err_sar_den );
    info->sar_height = sar_den;
    info->sar_width  = sar_num;
    if( vi->fpsNum == 0 && vi->fpsDen == 0 ) {
        /* There are no FPS data with native VFR videos, so let's grab it from first frame. */
        int err_num, err_den;
        int64_t fps_num = h->vsapi->propGetInt( props, "_DurationNum", 0, &err_num );
        int64_t fps_den = h->vsapi->propGetInt( props, "_DurationDen", 0, &err_den );
        FAIL_IF_ERROR( (err_num || err_den), "missing FPS values at frame 0" );
        FAIL_IF_ERROR( !fps_den, "FPS denominator is zero at frame 0" );
        info->fps_num = fps_den;
        info->fps_den = fps_num;
        /*
           Idk how to retrieve optimal timebase here, as we probably need extra path to read all frame props...
           Indeed that's redundant, so just hardcode the most common value. Hope that's enough.
        */
        info->timebase_num = h->timebase_num = 1;
        info->timebase_den = h->timebase_den = 10000000;
        h->current_timecode_num = 0;
        h->current_timecode_den = 1;
        info->vfr = h->vfr = 1;
    } else {
        info->fps_num = vi->fpsNum;
        info->fps_den = vi->fpsDen;
    }

    h->vsapi->freeFrame( frame0 ); // What a waste, but whatever...

    h->num_frames = info->num_frames = vi->numFrames;
    h->bit_depth = vi->format->bitsPerSample;
    FAIL_IF_ERROR( h->bit_depth < 8 || h->bit_depth > 16, "unsupported bit depth `%d'\n", h->bit_depth );
    FAIL_IF_ERROR( vi->format->sampleType == stFloat, "unsupported sample type `float'\n" );
    info->thread_safe = 1;

    h->async_requests = core_info->numThreads;
    h->async_buffer = (const VSFrameRef **)malloc(h->num_frames * sizeof(const VSFrameRef *));
    h->async_frame_done_event = (HANDLE*)malloc(h->num_frames * sizeof(h->async_frame_done_event));

    /* Set all frame event handles into "waiting" state. */
    for (int i = h->async_start_frame; i < h->num_frames; i++) {
        if ( NULL == (h->async_frame_done_event[i] = CreateEvent(NULL, FALSE, FALSE, NULL)) )
            FAIL_IF_ERROR( 1, "failed to create async event for frame %d\n", i );
    }

    const int intital_request_size = min(h->async_requests, h->num_frames - h->async_start_frame);
    h->async_requested = h->async_start_frame + intital_request_size;
    for ( int n = h->async_start_frame; n < h->async_start_frame + intital_request_size; n++ )
    {
        h->vsapi->getFrameAsync( n, h->node, async_callback, h );
        atomic_fetch_add( &h->async_pending, 1 );
    }

    h->uc_depth = (h->bit_depth & 7) && (vi->format->colorFamily == cmYUV || vi->format->colorFamily == cmYCoCg);

    if( vi->format->id == pfRGB48 )
        info->csp = X264_CSP_BGR | X264_CSP_VFLIP | X264_CSP_HIGH_DEPTH;
    else if( vi->format->id == pfRGB24 )
        info->csp = X264_CSP_BGR | X264_CSP_VFLIP;
    else if( vi->format->id == pfYUV444P9 || vi->format->id == pfYUV444P10 || vi->format->id == pfYUV444P12 || vi->format->id == pfYUV444P14 || vi->format->id == pfYUV444P16)
        info->csp = X264_CSP_I444 | X264_CSP_HIGH_DEPTH;
    else if( vi->format->id == pfYUV422P9 || vi->format->id == pfYUV422P10 || vi->format->id == pfYUV422P12 || vi->format->id == pfYUV422P14 || vi->format->id == pfYUV422P16)
        info->csp = X264_CSP_I422 | X264_CSP_HIGH_DEPTH;
    else if( vi->format->id == pfYUV420P9 || vi->format->id == pfYUV420P10 || vi->format->id == pfYUV420P12 || vi->format->id == pfYUV420P14 || vi->format->id == pfYUV420P16)
        info->csp = X264_CSP_I420 | X264_CSP_HIGH_DEPTH;
    else if( vi->format->id == pfYUV444P8 )
        info->csp = X264_CSP_I444;
    else if( vi->format->id == pfYUV422P8 )
        info->csp = X264_CSP_I422;
    else if( vi->format->id == pfYUV420P8 )
        info->csp = X264_CSP_I420;
    else
        FAIL_IF_ERROR( 1, "not supported pixel type: %s\n", vi->format->name );

    *p_handle = h;

    return 0;
}

static int picture_alloc( cli_pic_t *pic, hnd_t handle, int csp, int width, int height )
{
    if( x264_cli_pic_alloc( pic, X264_CSP_NONE, width, height ) )
        return -1;
    pic->img.csp = csp;
    const x264_cli_csp_t *cli_csp = x264_cli_get_csp( csp );
    if( cli_csp )
        pic->img.planes = cli_csp->planes;
    return 0;
}

static int read_frame( cli_pic_t *pic, hnd_t handle, int i_frame )
{
    static const int planes[3] = { 0, 1, 2 };
    VapourSynthContext *h = handle;

    if( i_frame >= h->num_frames )
        return -1;

    if( h->async_failed_frame >= i_frame )
        return -1;

    WaitForSingleObject( h->async_frame_done_event[i_frame], INFINITE );
    if( !h->async_buffer[i_frame] )
        return -1;
    pic->opaque = (VSFrameRef*)h->async_buffer[i_frame];

    /* Prefetch subsequent frames. */
    if (h->async_requested < h->num_frames)
    {
        while( h->async_requests >= (h->async_requested - h->async_consumed) && h->async_failed_frame < 0 )
        {
            h->vsapi->getFrameAsync( h->async_requested, h->node, async_callback, h );
            h->async_requested++;
            atomic_fetch_add( &h->async_pending, 1 );
        }
    }

    for( int i = 0; i < pic->img.planes; i++ )
    {
        const VSFormat *fi = h->vsapi->getFrameFormat( pic->opaque );
        pic->img.stride[i] = h->vsapi->getStride( pic->opaque, planes[i] );
        pic->img.plane[i] = (uint8_t*)h->vsapi->getReadPtr( pic->opaque, planes[i] );

        if( h->uc_depth )
        {
            /* Upconvert non 16-bit high depth planes to 16-bit
             * using the same algorithm as in the depth filter. */
            uint16_t *plane = (uint16_t*)pic->img.plane[i];
            int height = h->vsapi->getFrameHeight( pic->opaque, planes[i] );
            int pixel_count = pic->img.stride[i] / fi->bytesPerSample * height;
            int lshift = 16 - h->bit_depth;
            for( uint64_t j = 0; j < pixel_count; j++ )
                plane[j] = plane[j] << lshift;
        }
    }

    if ( h->vfr )
    {
        /* Adapted from vspipe timecodes generator and lavf.c vfr part. */
        pic->pts = ( h->current_timecode_num * h->timebase_den / h->current_timecode_den ); // hope it will fit
        pic->duration = 0;
        const VSMap *props = h->vsapi->getFramePropsRO( pic->opaque );
        int err_num, err_den;
        int64_t duration_num = h->vsapi->propGetInt( props, "_DurationNum", 0, &err_num );
        int64_t duration_den = h->vsapi->propGetInt( props, "_DurationDen", 0, &err_den );
        FAIL_IF_ERROR( (err_num || err_den), "missing duration at frame %d", i_frame );
        FAIL_IF_ERROR( !duration_den, "duration denominator is zero at frame %d", i_frame );
        vs_addRational( &h->current_timecode_num, &h->current_timecode_den, duration_num, duration_den );
    }

    h->vsapi->freeFrame( pic->opaque );
    h->async_buffer[i_frame] = NULL;
    h->async_consumed++;

    return 0;
}

static int release_frame( cli_pic_t *pic, hnd_t handle )
{
    /* Not reliable (frames doesn't freed on Ctrl+C event or
       script fail), so we're releasing them at read_frame. */
    (void)pic;
    (void)handle;
    return 0;
}

static void picture_clean( cli_pic_t *pic, hnd_t handle )
{
    memset( pic, 0, sizeof(cli_pic_t) );
}

static int close_file( hnd_t handle )
{
    VapourSynthContext *h = handle;

    /* Wait for any async requests to complete. */
    atomic_int out;
    while ( out = atomic_load( &h->async_pending ) ) {
        x264_cli_log( "vpy", X264_LOG_DEBUG, "waiting for %d async frame requests to complete...      \r", out );
        vs_sleep();
    }

    /* Release not consumed frames. Needed in case of early interruption
       or when --frames option is less than actual script's frame count. */
    for ( int i = h->async_consumed; i < h->async_requested; i++ )
    {
        if ( h->async_buffer[i] != NULL )
            h->vsapi->freeFrame( h->async_buffer[i] );
    }

    if( h->async_buffer )
        free( h->async_buffer );

    /* Event handles should be closed. */
    for ( int i = h->async_start_frame; i < h->num_frames; i++ )
    {
        if ( h->async_frame_done_event[i] )
            CloseEvent( h->async_frame_done_event[i] );
    }

    if( h->async_frame_done_event )
        free( h->async_frame_done_event );

    h->vsapi->freeNode( h->node );
    h->func.vsscript_freeScript( h->script );
    h->func.vsscript_finalize();

    if( h->library )
        vs_close( h->library );

    free( h );

    return 0;
}

const cli_input_t vpy_input = { open_file, picture_alloc, read_frame, release_frame, picture_clean, close_file };
