/*
     FreeSurround2020 Output Plugin

    Copyright (c) 2020 Brian Barnes
    Based on foo_dsp_freesurround (c) 2007-2010 Christian Kothe
      and FreeSurround Output Plugin (c) 2009 Michel Cailhol

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <string.h>
#define __USE_XOPEN
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <alsa/pcm_plugin.h>
#include "resource1.h"
#include "circ_buffer.hpp"
#include "stream_chunker.h"
#include "freesurround_decoder.h"
#include <boost/bind.hpp>
#include <boost/assign.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/format.hpp>
#include <functional>
#include <strstream>
#include <numeric>
#include <vector>
#include <queue>
#include <map>
#include <thread>


//ALSA plugin function definitions
#ifdef __cplusplus
extern "C" {
#endif
    // static snd_pcm_extplug_callback_t fs_callback;
    // static int fs_prepare(snd_pcm_extplug_t *ext);
    // static int fs_close(snd_pcm_extplug_t *ext);
    // static snd_pcm_sframes_t fs_transfer(snd_pcm_extplug_t *ext,
    //        const snd_pcm_channel_area_t *dst_areas,
    //        snd_pcm_uframes_t dst_offset,
    //        const snd_pcm_channel_area_t *src_areas,
    //        snd_pcm_uframes_t src_offset,
    //        snd_pcm_uframes_t size);
#ifdef __cplusplus
}
#endif

const unsigned int INPUT_CHANNELS = 2;
const int fs_to_alsa_table[8] = {0, 4, 1, 6, 7, 2, 3, 5};
const int alsa_to_fs_table[8] = {0, 2, 5, 6, 1, 7, 3, 4};
std::vector<int> alsa_to_fs(int num_channels) {
    std::vector<int> mapping;
    for (int i = 0; i < 8; i++) {
        int fs_channel = std::distance(alsa_to_fs_table, std::find(alsa_to_fs_table, alsa_to_fs_table+num_channels, i));
        if (fs_channel < num_channels) {
            mapping.push_back(fs_channel);
        }
    }
    return mapping;
}
std::thread *fs_thread;

// holds the user-configurable parameters of the FreeSurround plugin
struct freesurround_params
{
    // ALSA channel mappings
    enum ac
    {
        channel_front_left   = 1 << 0,
        channel_front_right  = 1 << 1,
        channel_back_left    = 1 << 2,
        channel_back_right   = 1 << 3,
        channel_front_center = 1 << 4,
        channel_lfe          = 1 << 5,
        channel_side_left    = 1 << 6,
        channel_side_right   = 1 << 7,
    };
    // the user-configurable parameters
    float center_image, shift, depth, circular_wrap, focus, front_sep, rear_sep, bass_lo, bass_hi;
    bool use_lfe;
    channel_setup channels_fs;		// FreeSurround channel setup

    // construct with defaults
    freesurround_params(): center_image(0.7), shift(0), depth(1), circular_wrap(90), focus(0), front_sep(1), rear_sep(1),
        bass_lo(40), bass_hi(90), use_lfe(false), channels_fs(cs_5point1) {}

    freesurround_params(float center_init,
                        float shift_init,
                        float depth_init,
                        float circular_wrap_init,
                        float focus_init,
                        float front_sep_init,
                        float rear_sep_init,
                        float bass_lo_init,
                        float bass_hi_init,
                        bool use_lfe_init,
                        channel_setup cs_init):
                            center_image(center_init),
                            shift(shift_init),
                            depth(depth_init),
                            circular_wrap(circular_wrap_init),
                            focus(focus_init),
                            front_sep(front_sep_init),
                            rear_sep(rear_sep_init),
                            bass_lo(bass_lo_init),
                            bass_hi(bass_hi_init),
                            use_lfe(use_lfe_init),
                            channels_fs(cs_init) {}
};

// the FreeSurround pcm class
class freesurround_pcm {
    enum { chunk_size = 4096 };
public:
    // construct the plugin instance from a preset
    freesurround_pcm(freesurround_params fs_params = freesurround_params()):
        params(fs_params),
        rechunker(boost::bind(&freesurround_pcm::process_chunk,this,_1),chunk_size*2),
        decoder(params.channels_fs,4096), srate(44100)
    {
        // set up decoder parameters according to preset params
        decoder.circular_wrap(params.circular_wrap);
        decoder.shift(params.shift);
        decoder.depth(params.depth);
        decoder.focus(params.focus);
        decoder.center_image(params.center_image);
        decoder.front_separation(params.front_sep);
        decoder.rear_separation(params.rear_sep);
        decoder.bass_redirection(params.use_lfe);
        decoder.low_cutoff(params.bass_lo/(srate/2.0));
        decoder.high_cutoff(params.bass_hi/(srate/2.0));
        rechunker.flush();
        channel_map = alsa_to_fs(num_channels());
    }

    // receive a chunk from ALSA and buffer it
    //TODO: Change foobar audio_chunk to alsa data format
    //TODO: Maybe write new class for audio chunk?
    bool get_chunk(float *input, snd_pcm_uframes_t size) {
        rechunker.append(input, size);
        return false;
    }

    std::vector<float> get_out_buf() {
        int send_size = out_buf.size();
        std::vector<float> send_out;
        send_out.insert(send_out.begin(), out_buf.begin(), out_buf.end());
        out_buf.clear();
        return send_out;
    }

    unsigned num_channels() {
        return decoder.num_channels(params.channels_fs);
    }
    // process and emit a chunk (called by the rechunker when it's time)
    //TODO: Replace foobar chunk with ALSA data
    void process_chunk(float *stereo) {
        // set sampling rate dependent parameters
        decoder.low_cutoff(params.bass_lo/(srate/2.0));
        decoder.high_cutoff(params.bass_hi/(srate/2.0));
        // decode original chunk into discrete multichannel
        float *src = decoder.decode(stereo);
        // copy the data into the output chunk (respecting the different channel orders in alsa and FS)
        unsigned channels = num_channels();
        for (unsigned s=0; s<chunk_size; s++){
            for (unsigned c=0; c<channels; c++) {
                out_buf.push_back(src[channel_map[c]+(s*channels)]);
            }
        }
    }

private:
    freesurround_params params;			// parameters
    stream_chunker<float> rechunker;	// gathers/splits the inbound data stream into equally-sized chunks
    freesurround_decoder decoder;		// the surround decoder
    unsigned srate;	             		// last known sampling rate
    std::vector<float> out_buf;			// the buffer where we store outgoing samples
    std::vector<int> channel_map;
};

static std::unordered_map<std::string, channel_setup> const table = {
    {"cs_stereo",channel_setup::cs_stereo},
    {"cs_3stereo",channel_setup::cs_3stereo},
    {"cs_5stereo",channel_setup::cs_5stereo},
    {"cs_4point1",channel_setup::cs_4point1},
    {"cs_5point1",channel_setup::cs_5point1},
    {"cs_6point1",channel_setup::cs_6point1},
    {"cs_7point1",channel_setup::cs_7point1},
    {"cs_7point1_panorama",channel_setup::cs_7point1_panorama},
    {"cs_7point1_tricenter",channel_setup::cs_7point1_tricenter},
    {"cs_8point1",channel_setup::cs_8point1},
    {"cs_9point1_densepanorama",channel_setup::cs_9point1_densepanorama},
    {"cs_9point1_wrap",channel_setup::cs_9point1_wrap},
    {"cs_11point1_densewrap",channel_setup::cs_11point1_densewrap},
    {"cs_13point1_totalwrap",channel_setup::cs_13point1_totalwrap},
    {"cs_16point1",channel_setup::cs_16point1},
    {"cs_legacy",channel_setup::cs_legacy}
};
channel_setup find_cs(std::string str) {
    if (auto it = table.find(str); it != table.end()) {
        return it->second;
    } else {
        return channel_setup::cs_stereo;
    }
}

struct fs_data {
    fs_data(snd_pcm_extplug_t *ext) : ext(*ext) {}
    freesurround_pcm* plugin;
    snd_pcm_extplug_t ext;
    snd_pcm_hw_params_t *hw_params;
    bool finish;
    circ_buffer<float> *in_buf;
    circ_buffer<float> *out_buf;
};

//Threaded decoding
int decode_thread(fs_data *data) {
    bool finish = false;
    while (!finish) {
        //Copy input buffer to chunker
        std::vector<float> in_buf = data->in_buf->multipop();
        float chunk[in_buf.size()];
        std::copy(in_buf.begin(), in_buf.end(), chunk);
        data->plugin->get_chunk(chunk, in_buf.size());

        //Copy fs output to output buffer
        data->out_buf->multipush(data->plugin->get_out_buf());

        finish = data->finish;
    }
    return 0;
}

/*
 * Helper functions
 */
static inline void *area_addr(const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset) {
    unsigned int bitofs = area->first + area->step * offset;
    return area->addr + bitofs / 8;
}

static inline unsigned int area_step(const snd_pcm_channel_area_t *area) {
    return area->step / 32;
}

#ifdef __cplusplus
extern "C" {
#endif
/*
 * transfer callback
 */
static snd_pcm_sframes_t fs_transfer(snd_pcm_extplug_t *ext,
           const snd_pcm_channel_area_t *dst_areas,
           snd_pcm_uframes_t dst_offset,
           const snd_pcm_channel_area_t *src_areas,
           snd_pcm_uframes_t src_offset,
           snd_pcm_uframes_t size)
{
    fs_data *data = (fs_data *)ext->private_data;
    unsigned OUTPUT_CHANNELS = data->plugin->num_channels();
    float *src[INPUT_CHANNELS], *dst[OUTPUT_CHANNELS];
    unsigned int src_step[INPUT_CHANNELS], dst_step[OUTPUT_CHANNELS], c, s;

    for (c = 0; c < INPUT_CHANNELS; c++) {
        src[c] = (float *)area_addr(src_areas + c, src_offset);
        src_step[c] = area_step(src_areas + c);
    }
    for (c = 0; c < OUTPUT_CHANNELS; c++) {
        dst[c] = (float *)area_addr(dst_areas + c, dst_offset);
        dst_step[c] = area_step(dst_areas + c);
    }

    std::vector<float> in_vec;
    for (s=0; s<size; s++) {
        for (c=0; c<INPUT_CHANNELS; c++){
            in_vec.push_back(*src[c]);
            src[c] += src_step[c];
        }
    }
    data->in_buf->multipush(in_vec);

    // Copy from output buffer into dst
    std::vector<float> out_vec = data->out_buf->multipop(size*OUTPUT_CHANNELS);
    int i=0;
    for (s=0; s<size; s++) {
        for (c=0; c<OUTPUT_CHANNELS; c++) {
            //*dst[c] = data->out_buf->pop();
            if (out_vec.size()) {*dst[c] = out_vec[i];} else {*dst[c] = 0.0;}
            dst[c] += dst_step[c];
            i++;
        }
    }

    return size;
}


/*
 * prepare callback
 *
 * Allocate internal buffers
 */
static int fs_prepare(snd_pcm_extplug_t *ext) {
    fs_data *data = (fs_data *)ext->private_data;
    data->in_buf = new circ_buffer<float>(1000000, 0.0);
    data->out_buf = new circ_buffer<float>(1000000, 0.0);
    fs_thread = new std::thread(decode_thread, data);
    return 0;
}

/*
 * close callback
 */
static int fs_close(snd_pcm_extplug_t *ext) {
    fs_data *data = (fs_data *)ext->private_data;
    data->finish = true;
    fs_thread->join();
    delete fs_thread;
    delete data->plugin;
    delete data->in_buf;
    delete data->out_buf;
    return 0;
}

/*
 * hw_params callback
 */
static int fs_hw_params(snd_pcm_extplug_t *ext,snd_pcm_hw_params_t *params) { return 0; }

/*
 * free callback
 */
static int fs_hw_free(snd_pcm_extplug_t *ext) { return 0; }

/*
 * dump callback
 */
static void fs_dump(snd_pcm_extplug_t *ext,snd_output_t *out) { return; }

/*
 * set_chmap callback
 */
static int fs_set_chmap(snd_pcm_extplug_t *ext,const snd_pcm_chmap_t *map) { return 0; }

/*
 * query_chmaps callback
 */
static snd_pcm_chmap_query_t ** fs_query_chmaps(snd_pcm_extplug_t *ext) { return NULL; }

/*
 * get_chmap callback
 */
static snd_pcm_chmap_t * fs_get_chmap(snd_pcm_extplug_t *ext) { return NULL; }

/*
 * callback table
 */
snd_pcm_extplug_callback_t fs_callback = {
    .transfer = fs_transfer,
    .close = fs_close,
    .hw_params = fs_hw_params,
    .hw_free = fs_hw_free,
    .dump = fs_dump,
    .init = fs_prepare,
    .query_chmaps = fs_query_chmaps,
    .get_chmap = fs_get_chmap,
    .set_chmap = fs_set_chmap
};

/*
 * Main entry point
 */
SND_PCM_PLUGIN_DEFINE_FUNC(freesurround2020)
{
    fs_data *data;
    snd_config_iterator_t i, next;
    snd_config_t *sconf = NULL;
    static const unsigned int chlist[2] = {4, 6};
    int err;
    unsigned int channels = 6;
    snd_pcm_format_t format = SND_PCM_FORMAT_FLOAT_LE;

    float center_image = 0.7;
    float shift = 0;
    float front_sep = 1;
    float rear_sep = 1;
    float depth = 1;
    float circular_wrap = 90;
    float focus = 0;
    float bass_lo = 40;
    float bass_hi = 90;
    bool use_lfe = false;
    channel_setup cs = cs_5point1;
    std::string cs_string = "";

    if (stream != SND_PCM_STREAM_PLAYBACK)
    {
        SNDERR("freesurround is only for playback");
        return -EINVAL;
    }

    snd_config_for_each(i, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;
        if (snd_config_get_id(n, &id) < 0)
            continue;
        if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
            continue;
        if (strcmp(id, "slave") == 0) {
            sconf = n;
            continue;
        }

        if (strcmp(id, "channels") == 0) {
            long val;
            if (snd_config_get_integer(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            channels = val;
            if (channels != 2 && channels != 4 && channels != 6) {
                SNDERR("channels must be 2, 4 or 6");
                return -EINVAL;
            }
            continue;
        }

        if (strcmp(id, "center_image") == 0) {
            double val;
            if (snd_config_get_real(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            center_image = (float)val;  // Set the presence of the front center channel(s). Value range: [0.0..1.0] – fully present at 1.0, fully replaced by left/right at 0.0 (default: 1). The default of 1.0 results in spec-conformant decoding ("movie mode") while a value of 0.7 is better suited for music reproduction (which is usually mixed without a center channel).
            if (center_image < 0.0 || center_image > 1.0) {
                SNDERR("center_image must be between 0.0 and 1.0");
                return -EINVAL;
            }
            continue;
        }
         if (strcmp(id, "shift") == 0) {
            double val;
            if (snd_config_get_real(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            shift = (float)val;  //  Allows to shift the soundfield forward or backward. Value range: [-1.0..+1.0]. 0 is no offset, positive values move the sound forward, negative values move it backwards. (default: 0)
            if (shift < -1.0 || shift > 1.0) {
                SNDERR("shift must be between -0.5 and 1.0");
                return -EINVAL;
            }
            continue;
        }
        if (strcmp(id, "front_separation") == 0) {
            double val;
            if (snd_config_get_real(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            front_sep = (float)val;  // Set the front stereo separation. Value range: [0.0..inf] – 1.0 is default, 0.0 is mono.
            if (front_sep < 0.0) {
                SNDERR("front_separation must be between 0.0 and infinity");
                return -EINVAL;
            }
            continue;
        }
        if (strcmp(id, "rear_separation") == 0) {
            double val;
            if (snd_config_get_real(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            rear_sep = (float)val;  // Set the rear stereo separation. Value range: [0.0..inf] – 1.0 is default, 0.0 is mono.
            if (rear_sep < 0.0) {
                SNDERR("rear_separation must be between 0.0 and infinity");
                return -EINVAL;
            }
            continue;
        }
        if (strcmp(id, "depth") == 0) {
            double val;
            if (snd_config_get_real(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            depth = (float)val;  // Allows to scale the soundfield backwards. Value range: [0.0..+5.0] – 0 is all compressed to the front, 1 is no change, 5 is scaled 5x backwards (default: 1)
            if (depth < 0.0 || depth > 5.0) {
                SNDERR("depth must be between 0.0 and 1.0");
                return -EINVAL;
            }
            continue;
        }
         if (strcmp(id, "circular_wrap") == 0) {
            double val;
            if (snd_config_get_real(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            circular_wrap = (float)val;  //  Allows to wrap the soundfield around the listener in a circular manner. Determines the angle of the frontal sound stage relative to the listener, in degrees. A setting of 90 corresponds to standard surround decoding, 180 stretches the front stage from ear to ear, 270 wraps it around most of the head. The side and rear content of the sound field is compressed accordingly behind the listerer. (default: 90, range: [0..360])
            if (circular_wrap < 0.0 || circular_wrap > 360.0) {
                SNDERR("circular_wrap must be between 0.0 and 360.0");
                return -EINVAL;
            }
            continue;
        }
        if (strcmp(id, "focus") == 0) {
            double val;
            if (snd_config_get_real(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            focus = (float)val;  // Allows to control the localization (i.e., focality) of sources. Value range: [-1.0..+1.0] – 0 means unchanged, positive means more localized, negative means more ambient (default: 0)
            if (focus < -1.0 || focus > 1.0) {
                SNDERR("focus must be between -1.0 and 1.0");
                return -EINVAL;
            }
            continue;
        }
        if (strcmp(id, "bass_lo") == 0) {
            double val;
            if (snd_config_get_real(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            bass_lo = (float)val;  // Set the lower end of the transition band, in Hz/Nyquist (default: 40/22050).
            if (bass_lo < 0.0) {
                SNDERR("bass_lo must be between 0.0 and infinity");
                return -EINVAL;
            }
            continue;
        }
        if (strcmp(id, "bass_hi") == 0) {
            double val;
            if (snd_config_get_real(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            bass_hi = (float)val;  // Set the upper end of the transition band, in Hz/Nyquist (default: 90/22050).
            if (bass_hi < 0.0) {
                SNDERR("bass_hi must be between 0.0 and infinity");
                return -EINVAL;
            }
            continue;
        }
         if (strcmp(id, "use_lfe") == 0) {
            double val;
            if (snd_config_get_real(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            if (val != 0.0 && val != 1.0) {
                SNDERR("use_lfe must be either 0 or 1");
                return -EINVAL;
            }
            use_lfe = (bool)val;  // Enable/disable LFE channel (default: false = disabled)
            continue;
        }
        if (strcmp(id, "channel_setup") == 0) {
            const char* val;
            if (snd_config_get_string(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            cs_string = std::string(val);  // Set the output channel setup from the FreeSurround enum. Default is 5.1 surround.
            cs = find_cs(cs_string);
            continue;
        }
        if (strcmp(id, "channels") == 0) {
            long val;
            if (snd_config_get_integer(n, &val) < 0) {
                SNDERR("Invalid type for %s", id);
                return -EINVAL;
            }
            channels = (int)val;  // Set the number of channels to upmix to. Default is 6 for 5.1 surround.
            if (channels < 1 || channels > 7) {
                SNDERR("channels must be between 1 and 7");
            }
        }

        SNDERR("Unknown field %s", id);
        return -EINVAL;
    }

    if (! sconf) {
        SNDERR("No slave configuration for freesurround pcm");
        return -EINVAL;
    }

    if (cs_string == "") {
        channel_setup choices[8] = {cs_stereo, cs_stereo, cs_3stereo, cs_4point1, cs_5point1, cs_5point1, cs_6point1, cs_7point1};
        cs = choices[channels-1];
    }

    data = (fs_data *) calloc(1, sizeof(*data));
    if (! data) {
        SNDERR("cannot allocate");
        return -ENOMEM;
    }

    freesurround_pcm* fs_plugin = new freesurround_pcm(freesurround_params(
        center_image, shift, depth, circular_wrap, focus, front_sep, rear_sep,
        bass_lo, bass_hi, use_lfe, cs));

    data->plugin = fs_plugin;
    data->ext.version = SND_PCM_IOPLUG_VERSION;
    data->ext.name = "FreeSurround2020 upmix plugin";
    data->ext.callback = &fs_callback;
    data->ext.private_data = data;

    err = snd_pcm_extplug_create(&data->ext, name, root, sconf, stream, mode);
    if (err < 0) {
        free(data);
        return err;
    }

    snd_pcm_extplug_set_param_minmax(&data->ext,
                     SND_PCM_EXTPLUG_HW_CHANNELS,
                     1, 8);
    if (channels)
        snd_pcm_extplug_set_slave_param_minmax(&data->ext,
                               SND_PCM_EXTPLUG_HW_CHANNELS,
                               channels, channels);
    else
        snd_pcm_extplug_set_slave_param_list(&data->ext,
                             SND_PCM_EXTPLUG_HW_CHANNELS,
                             2, chlist);
    snd_pcm_extplug_set_param(&data->ext, SND_PCM_EXTPLUG_HW_FORMAT,format);
    snd_pcm_extplug_set_slave_param(&data->ext, SND_PCM_EXTPLUG_HW_FORMAT,format);

    *pcmp = data->ext.pcm;

    return 0;

}

SND_PCM_PLUGIN_SYMBOL(freesurround2020);

#ifdef __cplusplus
}
#endif
