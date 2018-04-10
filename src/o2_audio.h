#pragma once

#include "o2.h"
#include "o2xx.h"

#include <unordered_map>
#include <string>
#include <cstdint>
#include <mutex>
#include <vector>

namespace o2 {
	namespace audio {
		using endpoint_id_t = o2::int64;

		struct stream_status {
			endpoint_id_t id;
			size_t sample_count;
		};

		class receiver  {
			std::unordered_map<endpoint_id_t, stream_status> streams;
			std::vector<float> ring_buffer;
			mutable std::mutex buffer_lock;

			bool has_sync = false;
			o2_time reference;
			size_t read_head = 0;
			const int sample_rate;

			enum summation_result {
				ok,
				partial_write,
				partially_old,
				partially_new,
				entirely_old,
				entirely_new 
			};

			summation_result sum(const float*& buffer, size_t& write_head, size_t& todo);
			void push(endpoint_id_t, const float* buffer, size_t samples);
			void sync(endpoint_id_t, o2_time time);

			size_t synchronize_in_buffer(o2_time);
			size_t unsafe_available() const;
		public:
			receiver(service&, int sample_rate, const std::string& endpoint_name, size_t buffer_len = 88200);
			size_t pull(float *into_buffer, size_t max_frames, int buffer_stride = 1, o2_time* buffer_start = nullptr);
			void gap(size_t);
			size_t available() const;
			int num_channels() const { return 1; }
			void set_stream_time(o2_time);
			bool empty() const;
		};

		class transmitter {
			client sender;
			endpoint_id_t id;
			o2_time time = 0.0;
			size_t sample_counter = 0;
			int sample_rate;
		public:
			transmitter(application&, int sample_rate, const std::string& endpoint_name);
			~transmitter();
			void set_stream_time(o2_time);
			size_t push(const float* from_buffer, size_t num_samples);
			void close();
			size_t get_sample_counter() const { return sample_counter; }
		};
	}
}
