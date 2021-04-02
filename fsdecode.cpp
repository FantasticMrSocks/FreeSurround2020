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
#include <fstream>
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
    int srate;
    bool use_lfe;
    channel_setup channels_fs;		// FreeSurround channel setup

    // construct with defaults
    freesurround_params(): center_image(0.7), shift(0), depth(1), circular_wrap(90), focus(0), front_sep(1), rear_sep(1),
        bass_lo(40), bass_hi(90), use_lfe(false), channels_fs(cs_5point1), srate(48000) {}

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
                        channel_setup cs_init,
                        int srate_init):
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
                            channels_fs(cs_init),
                            srate (srate_init) {}
};

// the FreeSurround wrapper class
class freesurround_wrapper {
    enum { chunk_size = 2048 };
public:
    // construct the wrapper instance from a preset
    freesurround_wrapper(freesurround_params fs_params = freesurround_params()):
        params(fs_params),
        rechunker(boost::bind(&freesurround_wrapper::process_chunk,this,_1),chunk_size*2),
        decoder(params.channels_fs,2048), srate(params.srate)
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
        float *chunk{ new float[temp_buf.size()] };
        std::copy(temp_buf.begin(), temp_buf.end(), chunk);
        wrapper->get_chunk(chunk, temp_buf.size());
        delete[] chunk;
        //Copy fs output to output buffer
        std::vector<float> fs_out_buf = wrapper->get_out_buf();
        out_buf->multipush(fs_out_buf);
        *finish = true;
    }
    return 0;
}

//Threaded output
void output_thread(threaded_circ_buffer<float> *out_buf, bool *finish) {

    while (!*finish) {

    }
    return;
}

argparse::ArgumentParser make_parser() {
    argparse::ArgumentParser parser("fsdecode");

    parser.add_argument("-v","--verbose")
        .help("Log extra information to the console\n")
        .default_value(false)
        .implicit_value(true);

    parser.add_argument("-i","--input")
        .help("A file to decode surround audio from. [default: stdin]")
        .nargs(1);

    parser.add_argument("-o","--output")
        .help("A file to write decoded audio to. [default: stdout]\n")
        .nargs(1);

    parser.add_argument("-B","--buffer_length")
        .help("The input buffer size, in samples. Increase this number if you encounter stuttering output.")
        .default_value(4096)
        .nargs(1)
        .action([](const std::string& value) {return std::stoi(value);});

    parser.add_argument("-c","--channels")
        .help("The number of audio channels to decode to.")
        .default_value(6)
        .nargs(1)
        .action([](const std::string& value) {return std::stoi(value);});

    parser.add_argument("-r","--samplerate")
        .help("The input sample rate, in Hz. [default: autodetect]")
        .nargs(1)
        .action([](const std::string& value) {return std::stoi(value);});

    parser.add_argument("-f","--format")
        .help("The input sample format. Choose from INT or FLOAT. [default: autodetect]")
        .nargs(1)
        .action([](const std::string& value) {
            static const std::vector<std::string> choices = {"INT","FLOAT"};
            if (std::find(choices.begin(),choices.end(),value)!=choices.end()) {
                return value;
            }
            return std::string{"auto"};
        });

    parser.add_argument("-b","--bits")
        .help("The input bits per sample. [default: autodetect]\n")
        .nargs(1)
        .action([](const std::string& value) {return std::stoi(value);});

    parser.add_argument("--focus")
        .help("Controls the localization of sources. Value range: [-1.0..+1.0] -- positive means more localized, negative means more ambient.")
        .default_value(0.0)
        .nargs(1)
        .action([](const std::string& value) {
            float f_value = std::stof(value);
            if ((f_value >= -1.0) && (f_value <= 1.0)) {
                return f_value;
            }
            return float(0.0);
        });

    parser.add_argument("--center_image")
        .help("Set the presence of the front center channel(s). Value range: [0.0..1.0] -- fully present at 1.0, fully replaced by left/right at 0.0.")
        .default_value(1.0)
        .nargs(1)
        .action([](const std::string& value) {
            static const float f_value = std::stof(value);
            if ((f_value >= 0.0) && (f_value <= 1.0)) {
                return f_value;
            }
            return float(1.0);
        });

    parser.add_argument("--circular_wrap")
        .help("Determines the angle of the frontal sound stage relative to the listener, in degrees. 90 corresponds to standard surround decoding, 180 stretches the front stage from ear to ear, 270 wraps it around most of the head. (range: [0..360])")
        .default_value(90.0)
        .nargs(1)
        .action([](const std::string& value) {
            static const float f_value = std::stof(value);
            if ((f_value >= 0.0) && (f_value <= 360.0)) {
                return f_value;
            }
            return float(90.0);
        });
    
    parser.add_argument("--shift")
        .help("Shifts the soundfield forward or backward. Value range: [-1.0..+1.0]. Positive moves the sound forward, negative moves it backwards.")
        .default_value(0.0)
        .nargs(1)
        .action([](const std::string& value) {
            static const float f_value = std::stof(value);
            if ((f_value >= -1.0) && (f_value <= 1.0)) {
                return f_value;
            }
            return float(0.0);
        });

    parser.add_argument("--depth")
        .help("Scales the soundfield backwards. Value range: [0.0..+5.0] -- 0 is all compressed to the front, 5 is scaled 5x backwards.")
        .default_value(1.0)
        .nargs(1)
        .action([](const std::string& value) {
            static const float f_value = std::stof(value);
            if ((f_value >= 0.0) && (f_value <= 5.0)) {
                return f_value;
            }
            return float(1.0);
        });

    parser.add_argument("--front_sep")
        .help("Sets the front stereo separation. Value range: [0.0..inf] -- 1.0 is default, 0.0 is mono.")
        .default_value(1.0)
        .nargs(1)
        .action([](const std::string& value) {
            static const float f_value = std::stof(value);
            if ((f_value >= 0.0)) {
                return f_value;
            }
            return float(1.0);
        });

    parser.add_argument("--rear_sep")
        .help("Sets the rear stereo separation. Value range: [0.0..inf] -- 1.0 is default, 0.0 is mono.")
        .default_value(1.0)
        .nargs(1)
        .action([](const std::string& value) {
            static const float f_value = std::stof(value);
            if ((f_value >= 0.0)) {
                return f_value;
            }
            return float(1.0);
        });

    parser.add_argument("--use_lfe")
        .help("Enable/disable LFE channel.")
        .default_value(true)
        .nargs(1)
        .action([](const std::string& value) {
            static const std::vector<std::string> choices = {"true","false"};
            if (std::find(choices.begin(),choices.end(),value)!=choices.end()) {
                if (value == "true") return true;
                return false;
            }
            return true;
        });

    parser.add_argument("--bass_lo")
        .help("Sets the lower end of the transition band, in Hz.")
        .default_value(40.0)
        .nargs(1)
        .action([](const std::string& value) {return std::stof(value);});

    parser.add_argument("--bass_hi")
        .help("Sets the upper end of the transition band, in Hz.")
        .default_value(90.0)
        .nargs(1)
        .action([](const std::string& value) {return std::stof(value);});

    return parser;
}

int main(int argc, const char *argv[]) {
    argparse::ArgumentParser parser = make_parser();

    try {
        parser.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cout << err.what() << std::endl;
        std::cout << parser;
        exit(0);
    }
    /*
    TODO: Read input from stdin

    TODO: Write data to stdout
    */
    
    // set up parameter values
    bool verbose = parser.get<bool>("--verbose");
    std::string input = "stdin";
    if (auto p_input = parser.present("--input")) {
        input = p_input.value();
    };
    std::string output = "stdout";
    if (auto p_output = parser.present("--output")) {
        output = p_output.value();
    }
    int buffer_length = parser.get<int>("--buffer_length");
    int samplerate = parser.present<int>("--samplerate").value_or(0);
    int bits = parser.present<int>("--bits").value_or(0);
    std::string format = "";
    if (auto p_format = parser.present<std::string>("--format")) {
        format = p_format.value();
    }
    int channels = parser.get<int>("--channels");
    float center_image = parser.get<double>("--center_image");
    float shift = parser.get<double>("--shift");
    float front_sep = parser.get<double>("--front_sep");
    float rear_sep = parser.get<double>("--rear_sep");
    float depth = parser.get<double>("--depth");
    float circular_wrap = parser.get<double>("--circular_wrap");
    float focus = parser.get<double>("--focus");
    float bass_lo = parser.get<double>("--bass_lo");
    float bass_hi = parser.get<double>("--bass_hi");
    bool use_lfe = parser.get<bool>("--use_lfe");

    // set up fsdecode data
    threaded_circ_buffer<float> *in_buf = new threaded_circ_buffer<float>;
    threaded_circ_buffer<float> *out_buf = new threaded_circ_buffer<float>;
    bool *finish = new bool(false);
    signal(2, [](int signum){
        std::cerr << "Received an interrupt signal" << std::endl;
        interrupt_received = true;
    });

    // If input is a file, load its contents into the input buffer
    if (input != "stdin") {
        AudioFile<float> in_file;
        in_file.load(input);

        samplerate = in_file.getSampleRate();
        bits = in_file.getBitDepth();

        int numChannels = in_file.getNumChannels();
        int numSamples = in_file.getNumSamplesPerChannel();
        in_buf->set_capacity(numChannels * numSamples);
        out_buf->set_capacity(3 * in_buf->capacity());

        for (int i = 0; i < numSamples; i++) {
            for (int j = 0; j < numChannels; j++) {
                in_buf->push(in_file.samples[j][i]);
            }
        }
    }

    // set up FreeSurround decoder
    channel_setup choices[8] = {cs_stereo, cs_stereo, cs_3stereo, cs_4point1, cs_5point1, cs_5point1, cs_6point1, cs_7point1};
    channel_setup cs = choices[channels-1];
    freesurround_wrapper *wrapper = new freesurround_wrapper(freesurround_params(
        center_image, shift, depth, circular_wrap, focus, front_sep, rear_sep,
        bass_lo, bass_hi, use_lfe, cs, samplerate));

    // log verbose output
    if (verbose) {
        std::cerr << "fsdecode - the standalone FreeSurround decoder" << std::endl << std::endl;
        std::cerr << "PCM details" << std::endl;
        std::cerr << "\tSample format: " << format << std::endl;
        std::cerr << "\tBid depth: " << bits << std::endl;
        std::cerr << "\tSample rate: " << samplerate << std::endl << std::endl;
        std::cerr << "FreeSurround parameters" << std::endl;
        std::cerr << "\tChannels: " << channels << std::endl;
        std::cerr << "\tCenter Image: " << center_image << std::endl;
        std::cerr << "\tShift: " << shift << std::endl;
        std::cerr << "\tFront Separation: " << front_sep << std::endl;
        std::cerr << "\tRear Separation: " << rear_sep << std::endl;
        std::cerr << "\tDepth: " << depth << std::endl;
        std::cerr << "\tCircular Wrap: " << circular_wrap << std::endl;
        std::cerr << "\tFocus: " << focus << std::endl;
        std::cerr << "\tBass Low Cutoff: " << bass_lo << std::endl;
        std::cerr << "\tBass High Cutoff: " << bass_hi << std::endl;
        std::cerr << "\tUse LFE: " << use_lfe << std::endl;
    }

    std::thread thread_in;
    std::thread thread_out;
    std::thread thread_decode;

    // Start threads
    if (output == "stdout") {thread_out = std::thread(output_thread, out_buf, finish);}
    thread_decode = std::thread(decode_thread, wrapper, in_buf, out_buf, finish);
    if (input == "stdin") {thread_in = std::thread(input_thread, in_buf, finish);}

    // Loop: wait for sigint/eof
    while (!*finish) {
        if (interrupt_received) *finish = true;
    }

    // Stop threads
    if (input == "stdin") {thread_in.join();}
    thread_decode.join();
    if (output == "stdout") {thread_out.join();}

    // If output is a file, copy the output buffer to that file
    if (output != "stdout") {
        AudioFile<float> out_file;

        int numSamples = out_buf->size();
        out_file.setNumChannels(channels);
        out_file.setNumSamplesPerChannel(numSamples/channels);
        out_file.setSampleRate(samplerate);
        out_file.setBitDepth(bits);

        for (int i = 0; i < out_file.getNumSamplesPerChannel(); i++) {
            for (int j = 0; j < out_file.getNumChannels(); j++) {
                out_file.samples[j][i] = out_buf->pop();
            }
        }

        out_file.save(output);
    }

    delete out_buf;
    delete in_buf;
    delete wrapper;
    delete finish;

   return 0;
}
