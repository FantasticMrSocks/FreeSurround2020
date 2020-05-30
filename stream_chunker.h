/*
Copyright (C) 2005 Christian Kothe

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

#ifndef STREAM_CHUNKER_H
#define STREAM_CHUNKER_H
#include <boost/function.hpp> // -> www.boost.org
#include <vector>
#include <iostream>

// accumulates/splits data blocks of arbitrary length into chunks of 
// specified length and passes them on to the given handler
template<class T>
class stream_chunker {
	// handles a fixed-length chunk
	typedef boost::function<void (T*)> chunk_handler;
public:
	// specify a chunk handler and the appropriate chunk length to feed it
	stream_chunker(const chunk_handler &handler, unsigned len): handle_chunk(handler), chunk_len(len) { }

	// flush the current buffer
	void flush() { buffer.clear(); }

	// get the amount of data kept in buffer
	unsigned buffered() { return (unsigned)buffer.size(); }

	// append a block of data
	void append(const std::vector<T> &data) { append(&data[0],data.size()); }
	template<unsigned N> void append(const T data[N]) { append(&data[0],N); }
	void append(T *data, unsigned n) {
		unsigned cp=0;
		// if we have elements in the current buffer, fill it up until it is full (then process it)
		if (buffer.size()) {
			unsigned u_size = buffer.size();
			std::cout << "chunk_buffer size: " + std::to_string(u_size) + "; ";
			unsigned delta = std::min(n,chunk_len-u_size);
			copy(&data[0],&data[delta],std::back_inserter(buffer));
			cp += delta;
			if (buffer.size() == chunk_len) {
				handle_chunk(&buffer[0]);
				buffer.clear();
			}
		}
		if (buffer.empty()) {
			// as long as we have more than one block, process it
			while (n-cp > chunk_len) {
				handle_chunk(&data[cp]); 
				cp+=chunk_len;
			}
			// put the rest into the buffer
			if (n>cp)
				buffer.assign(&data[cp],&data[n]);
		}
	}

private:
	chunk_handler handle_chunk; // handler that get called once a chunk is ready
	unsigned chunk_len;			// requested size of the chunks
	std::vector<T> buffer;	    // buffer used for storing incomplete chunks
};

#endif