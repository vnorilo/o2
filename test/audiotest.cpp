#define _USE_MATH_DEFINES

#include "o2xx.h"
#include "o2_audio.h"

#include <list>
#include <cmath>
#include <iostream>

o2::application app("audio");
int sample_rate = 44100;

const int test_length = 10000000;

int num_channels = 1;

std::vector<std::unique_ptr<o2::audio::receiver>> construct_receivers(std::string receiver_name) {
	std::vector<std::unique_ptr<o2::audio::receiver>> receivers;
	for (int i = 0;i < num_channels;++i) {
		receivers.emplace_back(std::make_unique<o2::audio::receiver>(app, receiver_name, sample_rate, std::to_string(i)));
	}
	return receivers;
}

std::vector<o2::audio::transmitter> construct_transmitters(std::string transmitter_name) {
	std::vector<o2::audio::transmitter> transmitters;
	for (int i = 0;i < num_channels;++i) {
		transmitters.emplace_back(app, transmitter_name, sample_rate, std::to_string(i));
	}
	return transmitters;
}

void loopback() {
	using namespace o2::audio;
	std::clog << "Starting loopback service\n";

	auto receivers = construct_receivers("server");
	auto transmitters = construct_transmitters("client");

	std::clog << "Waiting for clock synchronization... ";
	for (auto &t : transmitters) {
		t.wait_for_sync();
	}
	std::clog << "Ok!\n";

	std::vector<int> did(num_channels);
	std::vector<float> work;
	
	for (bool pending = true; pending;) {
		pending = false;
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

				if (did[i] < test_length) pending = true;
			} else pending = true;
		}
		std::this_thread::yield();
	}
}

void transmit() {
	using namespace o2::audio;

	auto receivers = construct_receivers("client");
	auto transmitters = construct_transmitters("server");

	std::vector<float> temp(1000);
	for (int i = 0;i < 1000;++i) {
		temp[i] = sinf(float(i * 0.1 * M_PI));
	}

	std::vector<int> sent(num_channels), received(num_channels);

	for (bool pending = true; pending;) {
		pending = false;
		int min_recv = received[0];
		for (int i = 0;i < num_channels;++i) {
			auto to_send = std::min((int)temp.size(), test_length - sent[i]);
			sent[i] += (int)transmitters[i].push(temp.data(), to_send);

			if (receivers[i]->is_connected()) {
				auto avail = receivers[i]->available();
				received[i] += (int)receivers[i]->drop(avail);

				if (received[i] < test_length) pending = true;
				min_recv = std::min(received[i], min_recv);
			} else pending = true;
		}
		if (sent[0] >= test_length) std::clog << "(Done Sending) ";
		std::clog << "Received " << min_recv << " / " << test_length << "\r";
	}
}

int main(int argn, const char* argv[]) {
	bool do_send, do_loop;
	
	do_send = do_loop = argn < 2;

	for (int i = 2;i < argn;++i) {
		if (!strcmp(argv[i], "send")) do_send = true;
		else if (!strcmp(argv[i], "loopback")) do_loop = true;
	}

	if (do_send) o2_clock_set(nullptr, nullptr);

	std::thread loopback_thread;
	if (do_loop) {
		loopback_thread = std::thread(loopback);
	}

	if (do_send) transmit();

	if (do_loop) {
		loopback_thread.join();
	}
	return 0;
}