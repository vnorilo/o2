#define _USE_MATH_DEFINES

#include "o2xx.h"
#include "o2_audio.h"

#include <list>
#include <cmath>
#include <iostream>
#include <chrono>

std::unique_ptr<o2::application> app;

int sample_rate = 44100;
int num_channels = 200;
volatile bool run = true;

std::vector<std::unique_ptr<o2::audio::receiver>> construct_receivers(std::string receiver_name) {
	std::vector<std::unique_ptr<o2::audio::receiver>> receivers;
	for (int i = 0;i < num_channels;++i) {
		receivers.emplace_back(std::make_unique<o2::audio::receiver>(*app, receiver_name, sample_rate, std::to_string(i)));
	}
	return receivers;
}

std::vector<o2::audio::transmitter> construct_transmitters(std::string transmitter_name) {
	std::vector<o2::audio::transmitter> transmitters;
	for (int i = 0;i < num_channels;++i) {
		transmitters.emplace_back(*app, transmitter_name, sample_rate, std::to_string(i));
	}
	return transmitters;
}

void loopback() {
	using namespace o2::audio;

	std::clog << "Starting loopback...\n";

	auto receivers = construct_receivers("client");
	auto transmitters = construct_transmitters("server");

	std::clog << "Waiting for sync on all channels ... ";
	for (auto &t : transmitters) {
		t.wait_for_sync();
	}
	std::clog << "Ok!\n\nProcessing... <Ctrl+C to terminate>";

	std::vector<int> did(num_channels);
	std::vector<float> work;
	
	for(;;) {
		for (int i = 0;i < num_channels;++i) {
			if (receivers[i]->is_connected()) {
				auto avail = receivers[i]->available();
				if (avail > work.size()) {
					work.resize(avail);
				}

				receivers[i]->pull(work.data(), avail);
				for (auto &s : work) s = -s;
				transmitters[i].push(work.data(), avail);

				did[i] += (int)avail;
			} else if (did[i]) {
				return;
			}
		}
	}
}

int num_tests = 1000;
int buffer_size = 1000;

void transmit() {
	auto receivers = construct_receivers("server");
	auto transmitters = construct_transmitters("client");

	std::vector<float> temp(buffer_size);
	for (int i = 0;i < temp.size();++i) {
		temp[i] = sinf(float(i * 0.1 * M_PI));
	}

	using clock_t = std::chrono::high_resolution_clock;
	using measurement_t = clock_t::duration;

	std::vector<measurement_t> send_time, recv_time, total_time;
    
    send_time.reserve(num_tests);
    recv_time.reserve(num_tests);
    total_time.reserve(num_tests);

	std::vector<int> received(num_channels);

	std::clog << "Waiting for time sync on all channels... ";
	for (auto &t : transmitters) t.wait_for_sync();
	std::clog << "Ok!\n\n";

	for (int i = 0;i < num_tests;++i) {
		auto start = clock_t::now();
		for (auto &r : transmitters) {
			r.push(temp.data(), temp.size());
		}
        auto sent = std::chrono::time_point_cast<std::chrono::nanoseconds>(clock_t::now());

		for (auto &r : received) r = 0;
		for (;;) {
			bool pending = false;
			for (int i = 0;i < num_channels;++i) {
				if (receivers[i]->is_connected()) {
					auto avail = receivers[i]->available();
					received[i] += (int)receivers[i]->drop(std::min(temp.size(), avail));
					if (received[i] < temp.size()) pending = true;
				} else pending = true;
			}
			if (!pending) break;
		}

		auto received = std::chrono::time_point_cast<std::chrono::nanoseconds>(clock_t::now());

		send_time.emplace_back(sent - start);
		recv_time.emplace_back(received - sent);
        total_time.emplace_back(received - start);
	}
    
    

	std::clog << "Median send time   : " << double(send_time[send_time.size() / 2].count()) / 1000. << "us\n"
	   		  << "Median recv latency: " << double(recv_time[recv_time.size() / 2].count()) / 1000. << "us\n"
              << "Median roundtrip   : " << double(total_time[total_time.size() / 2].count()) / 1000. << "us\n";

	run = false;
}

int main(int argn, const char* argv[]) {
	bool do_send, do_loop;
    
    if (getenv("O2_AUDIO_CHANNELS")) {
        num_channels = strtol(getenv("O2_AUDIO_CHANNELS"), nullptr, 10);
    }
    
    app = std::make_unique<o2::application>("app", 100);
	
	do_send = do_loop = argn < 2;

	for (int i = 1;i < argn;++i) {
		if (!strcmp(argv[i], "send")) do_send = true;
		else if (!strcmp(argv[i], "loopback")) do_loop = true;
	}

	if (do_send) {
		std::clog << "Providing master clock\n";
		o2_clock_set(nullptr, nullptr);
	}

	std::thread loopback_thread;
	if (do_loop) {
		if (do_send) {
			loopback_thread = std::thread(loopback);
		}
		else {
			loopback();
			return 0;
		}
	}

	if (do_send) transmit();

	if (do_loop) {
		loopback_thread.join();
	}
	return 0;
}
