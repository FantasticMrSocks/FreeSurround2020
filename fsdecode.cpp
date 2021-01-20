/*
    fsdecode - Standalone FreeSurround Decoder

    Copyright (c) 2021 Brian Barnes

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 3
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
#include <unistd.h>
#include "threaded_circ_buffer.hpp"
#include "FreeSurround/stream_chunker.h"
#include "FreeSurround/freesurround_decoder.h"
#include "AudioFile/AudioFile.h"
#include "ArgumentParser/argparse.hpp"
#include <boost/bind.hpp>
#include <boost/assign.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/format.hpp>
#include <vector>
#include <map>
#include <thread>
#include <iostream>
#include <csignal>

const unsigned int INPUT_CHANNELS = 2;
const int fs_to_alsa_table[8] = {0, 4, 1, 6, 7, 2, 3, 5};
std::vector<int> fs_to_alsa(int num_channels) {
    std::vector<int> table;
    for (int i = 0; i < 8; i++) {
        if (fs_to_alsa_table[i] < num_channels) table.push_back(fs_to_alsa_table[i]);
    }
    std::vector<int> mapping;
    for (int i = 0; i < num_channels; i++) {
        mapping.push_back(std::distance(table.begin(), std::find(table.begin(), table.end(), i)));
    }
    return mapping;
}

unsigned long get_mult(uint32_t bits) {
    long mult = 1;
    if (bits == 16) {
        mult = 32767;
    } else if (bits == 32) {
        mult = 2147483647;
    }
    return mult;
}

signed long to_s(float val, uint32_t bits) {
    long result = val * get_mult(bits);
    return result;
}

float to_f(signed long val, uint32_t bits) {
    float result = val / get_mult(bits);
    if (result < -1.0) result = -1.0;
    if (result > 1.0) result = 1.0;
    return result;
}

bool interrupt_received = false;

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
class freesurround_wrapper {
    enum { chunk_size = 2048 };
public:
    // construct the wrapper instance from a preset
    freesurround_wrapper(freesurround_params fs_params = freesurround_params()):
        params(fs_params),
        rechunker(boost::bind(&freesurround_wrapper::process_chunk,this,_1),chunk_size*2),
        decoder(params.channels_fs,2048), srate(48000)
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
        channel_map = fs_to_alsa(num_channels());
    }

    // receive a chunk and buffer it
    bool get_chunk(float *input, uint32_t size) {
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

//Threaded input
void input_thread(threaded_circ_buffer<float> *in_buf, bool *finish) {
    while (!*finish) {

    }
    return;
}

//Threaded decoding
int decode_thread(freesurround_wrapper *wrapper, threaded_circ_buffer<float> *in_buf, threaded_circ_buffer<float> *out_buf, bool *finish) {
    while (!*finish) {
        //Copy input buffer to chunker
        std::vector<float> temp_buf = in_buf->multipop();
        float chunk[temp_buf.size()];
        std::copy(temp_buf.begin(), temp_buf.end(), chunk);
        wrapper->get_chunk(chunk, temp_buf.size());

        //Copy fs output to output buffer
        std::vector<float> fs_out_buf = wrapper->get_out_buf();
        out_buf->multipush(fs_out_buf);
    }
    return 0;
}

//Threaded output
void output_thread(threaded_circ_buffer<float> *out_buf, bool *finish) {
    while (!*finish) {

    }
    return;
}

int main(int argc, const char** argv) {
    argparse::ArgumentParser parser;
    
    parser.addArgument("-h","--help");
    parser.addArgument("-c","--channels", 1);
    parser.addArgument("-r","--samplerate", 1);
    parser.addArgument("-f","--format", 1);
    parser.addArgument("-b","--bits", 1);
    parser.addArgument("-B","--buffer_length", 1);
    parser.addArgument("-i","--input", 1);
    parser.addArgument("-o","--output", 1);
    parser.addArgument("--center_image", 1);
    parser.addArgument("--shift", 1);
    parser.addArgument("--front_sep", 1);
    parser.addArgument("--rear_sep", 1);
    parser.addArgument("--depth", 1);
    parser.addArgument("--circular_wrap", 1);
    parser.addArgument("--bass_lo", 1);
    parser.addArgument("--bass_hi", 1);
    parser.addArgument("--focus", 1);
    parser.addArgument("--use_lfe", 1);
    parser.addArgument("--channel_setup", 1);

    parser.parse(argc, argv);

    /*
    TODO: Read input from stdin/infile

    TODO: Use AudioFile lib to parse wav data

    TODO: Calculate decode buffer size from sample rate

    TODO: Send data to chunker

    TODO: Receive data from decoder

    TODO: Write data to stdout/outfile

    TODO: Catch sigterm, write file size to header if writing to file
    */

    unsigned int channels = 6;
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

    if (cs_string == "") {
        channel_setup choices[8] = {cs_stereo, cs_stereo, cs_3stereo, cs_4point1, cs_5point1, cs_5point1, cs_6point1, cs_7point1};
        cs = choices[channels-1];
    }

    freesurround_wrapper *wrapper = new freesurround_wrapper(freesurround_params(
        center_image, shift, depth, circular_wrap, focus, front_sep, rear_sep,
        bass_lo, bass_hi, use_lfe, cs));
    threaded_circ_buffer<float> *in_buf = new threaded_circ_buffer<float>;
    threaded_circ_buffer<float> *out_buf = new threaded_circ_buffer<float>;
    bool *finish = new bool(false);
    signal(2, [](int signum){interrupt_received = true;});

    // Start threads
    std::thread thread_out = std::thread(output_thread, out_buf, finish);
    std::thread thread_decode = std::thread(decode_thread, wrapper, in_buf, out_buf, finish);
    std::thread thread_in = std::thread(input_thread, in_buf, finish);

    // Loop: wait for sigint/eof
    while (!*finish) {
        if (interrupt_received) *finish = true;
    }

    // Stop threads
    thread_in.join();
    thread_decode.join();
    thread_out.join();

    delete out_buf;
    delete in_buf;
    delete wrapper;
    delete finish;

   return 0;
}
