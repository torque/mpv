/*
 * JACK audio output driver for MPlayer
 *
 * Copyleft 2001 by Felix Bünemann (atmosfear@users.sf.net)
 * and Reimar Döffinger (Reimar.Doeffinger@stud.uni-karlsruhe.de)
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * along with MPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "core/mp_msg.h"

#include "ao.h"
#include "audio/format.h"
#include "osdep/timer.h"
#include "core/subopt-helper.h"

#include "libavutil/fifo.h"

#include <jack/jack.h>

//! maximum number of channels supported, avoids lots of mallocs
#define MAX_CHANS MP_NUM_CHANNELS
static jack_port_t * ports[MAX_CHANS];
static int num_ports; ///< Number of used ports == number of channels
static jack_client_t *client;
static float jack_latency;
static int estimate;
static volatile int paused = 0; ///< set if paused
static volatile int underrun = 0; ///< signals if an underrun occured

static volatile float callback_interval = 0;
static volatile float callback_time = 0;

//! size of one chunk, if this is too small MPlayer will start to "stutter"
//! after a short time of playback
#define CHUNK_SIZE (16 * 1024)
//! number of "virtual" chunks the buffer consists of
#define NUM_CHUNKS 8

//! buffer for audio data
static AVFifoBuffer *buffer;

/**
 * \brief insert len bytes into buffer
 * \param data data to insert
 * \param len length of data
 * \return number of bytes inserted into buffer
 *
 * If there is not enough room, the buffer is filled up
 */
static int write_buffer(unsigned char *data, int len)
{
    int free = av_fifo_space(buffer);
    if (len > free)
        len = free;
    return av_fifo_generic_write(buffer, data, len, NULL);
}

static void silence(float **bufs, int cnt, int num_bufs);

struct deinterleave {
    float **bufs;
    int num_bufs;
    int cur_buf;
    int pos;
};

static void deinterleave(void *info, void *src, int len)
{
    struct deinterleave *di = info;
    float *s = src;
    int i;
    len /= sizeof(float);
    for (i = 0; i < len; i++) {
        di->bufs[di->cur_buf++][di->pos] = s[i];
        if (di->cur_buf >= di->num_bufs) {
            di->cur_buf = 0;
            di->pos++;
        }
    }
}

/**
 * \brief read data from buffer and splitting it into channels
 * \param bufs num_bufs float buffers, each will contain the data of one channel
 * \param cnt number of samples to read per channel
 * \param num_bufs number of channels to split the data into
 * \return number of samples read per channel, equals cnt unless there was too
 *         little data in the buffer
 *
 * Assumes the data in the buffer is of type float, the number of bytes
 * read is res * num_bufs * sizeof(float), where res is the return value.
 * If there is not enough data in the buffer remaining parts will be filled
 * with silence.
 */
static int read_buffer(float **bufs, int cnt, int num_bufs)
{
    struct deinterleave di = {
        bufs, num_bufs, 0, 0
    };
    int buffered = av_fifo_size(buffer);
    if (cnt * sizeof(float) * num_bufs > buffered) {
        silence(bufs, cnt, num_bufs);
        cnt = buffered / sizeof(float) / num_bufs;
    }
    av_fifo_generic_read(buffer, &di, cnt * num_bufs * sizeof(float),
                         deinterleave);
    return cnt;
}

// end ring buffer stuff

/**
 * \brief fill the buffers with silence
 * \param bufs num_bufs float buffers, each will contain the data of one channel
 * \param cnt number of samples in each buffer
 * \param num_bufs number of buffers
 */
static void silence(float **bufs, int cnt, int num_bufs)
{
    int i;
    for (i = 0; i < num_bufs; i++)
        memset(bufs[i], 0, cnt * sizeof(float));
}

/**
 * \brief JACK Callback function
 * \param nframes number of frames to fill into buffers
 * \param arg unused
 * \return currently always 0
 *
 * Write silence into buffers if paused or an underrun occured
 */
static int outputaudio(jack_nframes_t nframes, void *arg)
{
    struct ao *ao = arg;
    float *bufs[MAX_CHANS];
    int i;
    for (i = 0; i < num_ports; i++)
        bufs[i] = jack_port_get_buffer(ports[i], nframes);
    if (paused || underrun || !buffer)
        silence(bufs, nframes, num_ports);
    else if (read_buffer(bufs, nframes, num_ports) < nframes)
        underrun = 1;
    if (estimate) {
        float now = mp_time_us() / 1000000.0;
        float diff = callback_time + callback_interval - now;
        if ((diff > -0.002) && (diff < 0.002))
            callback_time += callback_interval;
        else
            callback_time = now;
        callback_interval = (float)nframes / (float)ao->samplerate;
    }
    return 0;
}

/**
 * \brief print suboption usage help
 */
static void print_help(void)
{
    mp_msg(
        MSGT_AO, MSGL_FATAL,
        "\n-ao jack commandline help:\n"
        "Example: mpv -ao jack:port=myout\n"
        "  connects mpv to the jack ports named myout\n"
        "\nOptions:\n"
        "  connect\n"
        "    Automatically connect to output ports\n"
        "  port=<port name>\n"
        "    Connects to the given ports instead of the default physical ones\n"
        "  name=<client name>\n"
        "    Client name to pass to JACK\n"
        "  estimate\n"
        "    Estimates the amount of data in buffers (experimental)\n"
        "  autostart\n"
        "    Automatically start JACK server if necessary\n"
        );
}

static int init(struct ao *ao, char *params)
{
    const char **matching_ports = NULL;
    char *port_name = NULL;
    char *client_name = NULL;
    int autostart = 0;
    int connect = 1;
    const opt_t subopts[] = {
        {"port", OPT_ARG_MSTRZ, &port_name, NULL},
        {"name", OPT_ARG_MSTRZ, &client_name, NULL},
        {"estimate", OPT_ARG_BOOL, &estimate, NULL},
        {"autostart", OPT_ARG_BOOL, &autostart, NULL},
        {"connect", OPT_ARG_BOOL, &connect, NULL},
        {NULL}
    };
    jack_options_t open_options = JackUseExactName;
    int port_flags = JackPortIsInput;
    int i;
    estimate = 1;
    if (subopt_parse(params, subopts) != 0) {
        print_help();
        return -1;
    }

    struct mp_chmap_sel sel = {0};
    mp_chmap_sel_add_waveext(&sel);
    if (!ao_chmap_sel_adjust(ao, &sel, &ao->channels))
        goto err_out;

    if (!client_name) {
        client_name = malloc(40);
        sprintf(client_name, "mpv [%d]", getpid());
    }
    if (!autostart)
        open_options |= JackNoStartServer;
    client = jack_client_open(client_name, open_options, NULL);
    if (!client) {
        mp_msg(MSGT_AO, MSGL_FATAL, "[JACK] cannot open server\n");
        goto err_out;
    }
    jack_set_process_callback(client, outputaudio, ao);

    // list matching ports if connections should be made
    if (connect) {
        if (!port_name)
            port_flags |= JackPortIsPhysical;
        matching_ports = jack_get_ports(client, port_name, NULL, port_flags);
        if (!matching_ports || !matching_ports[0]) {
            mp_msg(MSGT_AO, MSGL_FATAL, "[JACK] no physical ports available\n");
            goto err_out;
        }
        i = 1;
        num_ports = ao->channels.num;
        while (matching_ports[i])
            i++;
        if (num_ports > i)
            num_ports = i;
    }

    // create out output ports
    for (i = 0; i < num_ports; i++) {
        char pname[30];
        snprintf(pname, 30, "out_%d", i);
        ports[i] =
            jack_port_register(client, pname, JACK_DEFAULT_AUDIO_TYPE,
                               JackPortIsOutput,
                               0);
        if (!ports[i]) {
            mp_msg(MSGT_AO, MSGL_FATAL, "[JACK] not enough ports available\n");
            goto err_out;
        }
    }
    if (jack_activate(client)) {
        mp_msg(MSGT_AO, MSGL_FATAL, "[JACK] activate failed\n");
        goto err_out;
    }
    for (i = 0; i < num_ports; i++) {
        if (jack_connect(client, jack_port_name(ports[i]),
                         matching_ports[i])) {
            mp_msg(MSGT_AO, MSGL_FATAL, "[JACK] connecting failed\n");
            goto err_out;
        }
    }
    ao->samplerate = jack_get_sample_rate(client);
    jack_latency_range_t jack_latency_range;
    jack_port_get_latency_range(ports[0], JackPlaybackLatency,
                                &jack_latency_range);
    jack_latency = (float)(jack_latency_range.max + jack_get_buffer_size(client))
                   / (float)ao->samplerate;
    callback_interval = 0;

    if (!ao_chmap_sel_get_def(ao, &sel, &ao->channels, num_ports))
        goto err_out;

    ao->format = AF_FORMAT_FLOAT_NE;
    ao->bps = ao->channels.num * ao->samplerate * sizeof(float);
    int unitsize = ao->channels.num * sizeof(float);
    ao->outburst = CHUNK_SIZE / unitsize * unitsize;
    ao->buffersize = NUM_CHUNKS * ao->outburst;
    buffer = av_fifo_alloc(ao->buffersize);
    free(matching_ports);
    free(port_name);
    free(client_name);
    return 0;

err_out:
    free(matching_ports);
    free(port_name);
    free(client_name);
    if (client)
        jack_client_close(client);
    av_fifo_free(buffer);
    buffer = NULL;
    return -1;
}

static float get_delay(struct ao *ao)
{
    int buffered = av_fifo_size(buffer); // could be less
    float in_jack = jack_latency;
    if (estimate && callback_interval > 0) {
        float elapsed = mp_time_us() / 1000000.0 - callback_time;
        in_jack += callback_interval - elapsed;
        if (in_jack < 0)
            in_jack = 0;
    }
    return (float)buffered / (float)ao->bps + in_jack;
}

/**
 * \brief stop playing and empty buffers (for seeking/pause)
 */
static void reset(struct ao *ao)
{
    paused = 1;
    av_fifo_reset(buffer);
    paused = 0;
}

// close audio device
static void uninit(struct ao *ao, bool immed)
{
    if (!immed)
        mp_sleep_us(get_delay(ao) * 1000 * 1000);
    // HACK, make sure jack doesn't loop-output dirty buffers
    reset(ao);
    mp_sleep_us(100 * 1000);
    jack_client_close(client);
    av_fifo_free(buffer);
    buffer = NULL;
}

/**
 * \brief stop playing, keep buffers (for pause)
 */
static void audio_pause(struct ao *ao)
{
    paused = 1;
}

/**
 * \brief resume playing, after audio_pause()
 */
static void audio_resume(struct ao *ao)
{
    paused = 0;
}

static int get_space(struct ao *ao)
{
    return av_fifo_space(buffer);
}

/**
 * \brief write data into buffer and reset underrun flag
 */
static int play(struct ao *ao, void *data, int len, int flags)
{
    if (!(flags & AOPLAY_FINAL_CHUNK))
        len -= len % ao->outburst;
    underrun = 0;
    return write_buffer(data, len);
}

const struct ao_driver audio_out_jack = {
    .info = &(const struct ao_info) {
        "JACK audio output",
        "jack",
        "Reimar Döffinger <Reimar.Doeffinger@stud.uni-karlsruhe.de>",
        "based on ao_sdl.c"
    },
    .init      = init,
    .uninit    = uninit,
    .get_space = get_space,
    .play      = play,
    .get_delay = get_delay,
    .pause     = audio_pause,
    .resume    = audio_resume,
    .reset     = reset,
};
