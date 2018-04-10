#include "o2_audio.h"

#include <algorithm>

#ifndef NDEBUG
#include <iostream>
#endif

namespace o2 {
	namespace audio {
		receiver::receiver(service& service, int sr, const std::string& endpoint, size_t buffer_size)
			:ring_buffer(buffer_size)
			,sample_rate(sr) {

			service.implement(endpoint + "/sync", "ht", "Receives stream id and sets stream time.", [this](o2_arg_ptr* argv, int argn) {
				sync(argv[0]->h, argv[1]->t);
			});

			service.implement(endpoint + "/push", "hvf", "Receives stream id and a vector of floating point samples.", [this](o2_arg_ptr* argv, int argn) {
				auto vector = argv[1]->v;
				push(argv[0]->h, vector.vf, vector.len);
			});

			service.implement(endpoint + "/close", "h", "Stops reading stream id.", [this](o2_arg_ptr* argv, int argn) {
				std::lock_guard<std::mutex> lg(this->buffer_lock);
#ifndef NDEBUG
				std::clog << "* Stream " << argv[0]->h << " closed\n";
#endif
				this->streams.erase(argv[0]->h);
			});
		}

		void receiver::sync(endpoint_id_t id, o2_time time) {
			std::lock_guard<std::mutex> lg(buffer_lock);
			streams[id].sample_count = synchronize_in_buffer(time);
#ifndef NDEBUG
			std::clog << "* Stream " << id << " starting at " << time << " : " << streams[id].sample_count << "\n";
#endif
		}

		bool receiver::empty() const {
			std::lock_guard<std::mutex> lg(buffer_lock);
			return streams.empty();
		}

		void receiver::push(endpoint_id_t id, const float* buffer, size_t samples) {
			while (samples) {
				std::lock_guard<std::mutex> lg(buffer_lock);
				switch (sum(buffer, streams[id].sample_count, samples)) {
				case entirely_old:
				case entirely_new:
					streams[id].sample_count += samples;
					return;
				case partial_write:
					break;
				case partially_old:
				case partially_new:
				case ok:
					return;
				}
			}
		}

		receiver::summation_result receiver::sum(const float*& buffer, size_t& write_head, size_t& todo) {
			size_t buffer_limit = read_head + ring_buffer.size();
			size_t write_limit = write_head + todo;

			// write totally out of buffer window?
			if (write_limit < read_head) {
				return entirely_old;
			}

			if (write_head > buffer_limit) {
				return entirely_new;
			}

			auto status = ok;

			// discard samples that are too late to be read
			if (write_head < read_head) {
				buffer += read_head - write_head;
				todo -= read_head - write_head;
				write_head = read_head;
				status = partially_old;
			}

			// determine contiguous write region
			size_t physical_write = write_head % ring_buffer.size();
			size_t physical_write_end = std::min(physical_write + todo, ring_buffer.size());

			// discard samples that would overflow the read head
			if (write_head + physical_write_end - physical_write > buffer_limit - 1) {
				status = partially_new;
				physical_write_end = physical_write + buffer_limit - 1 - write_head;
			}


			// sum into buffer
			todo -= physical_write_end - physical_write;
			write_head += physical_write_end - physical_write;
			while (physical_write < physical_write_end) {
				ring_buffer[physical_write++] += *buffer++;
			}

			return todo == 0 ? status : partial_write;
		}

		size_t receiver::synchronize_in_buffer(o2_time time) {
			if (has_sync) {
				auto delta = time - reference;
				auto pos = size_t(delta * sample_rate);
				return pos;
			} else {
				reference = time;
				read_head = 0; //size_t(sample_rate * (o2_time_get() - reference));
				has_sync = true;
				return 0;
			}
		}

		void receiver::set_stream_time(o2_time time) {
			synchronize_in_buffer(time);
		}

		size_t receiver::unsafe_available() const {
			auto avail = ring_buffer.size();
			for (auto &s : streams) {
				avail = std::min(avail, s.second.sample_count - read_head);
			}
			return avail;
		}

		size_t receiver::available() const {
			std::lock_guard<std::mutex> lg(buffer_lock);
			return unsafe_available();
		}
		
		void receiver::gap(size_t discard) {
			std::lock_guard<std::mutex> lg(buffer_lock);
			read_head += discard;
		}

		size_t receiver::pull(float *into_buffer, size_t max_frames, int stride, o2_time* buffer_start) {
			std::lock_guard<std::mutex> lg(buffer_lock);
			if (buffer_start) {
				*buffer_start = reference + double(read_head) / double(sample_rate);
			}

			// how much do we have?
			auto avail = unsafe_available();
			auto cando = std::min(max_frames, avail);
			auto todo = cando;

			while (todo) {
				// determine contiguous read region
				size_t physical_read = read_head % ring_buffer.size();
				size_t physical_read_end = std::min(physical_read + todo, ring_buffer.size());

				// copy into buffer and release
				auto read = physical_read_end - physical_read;
				while (physical_read < physical_read_end) {
					float s = 0;
					std::swap(s, ring_buffer[physical_read++]);
					*into_buffer = s;
					into_buffer += stride;
				}
				read_head += read;
				todo -= read;
			} 

			return cando;
		}

		transmitter::transmitter(application& app, int sample_rate, const std::string& endpoint):sender(app.request(endpoint)),sample_rate(sample_rate) {
			id = std::hash<void*>()(this) ^
				std::hash<std::string>()(app.get_reply_address());
		}

		static std::mutex l;

		void transmitter::set_stream_time(o2_time t) {
			time = t;
			sample_counter = 0;
			sender.send(time - 0.1, "sync", id, time);
		}

		size_t transmitter::push(const float* buffer, size_t len) {
			if (!len) return len;
			// provide a STL-like interface into pointer/length pair
			struct view {
				const float* buffer;
				size_t len;
				const float* data() const { return buffer; }
				const size_t size() const { return len; }
			};

			double stream_time = time + double(sample_counter) / double(sample_rate);
			// schedule message for delivery 100ms before the time stamp
			sender.send(stream_time - 0.1, "push", id, view{ buffer, len });
			sample_counter += len;
			return len;
		}

		void transmitter::close() {
			double stream_time = time + double(sample_counter) / double(sample_rate);
			sender.send(stream_time, "close", id);
		}

		transmitter::~transmitter() {
			close();
		}
	}
}