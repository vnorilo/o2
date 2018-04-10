#include "o2xx.h"

#include <iostream>

int main() {
	using namespace o2;
	application app{ "test" };
	auto my_service = app.provide("service");

	my_service.implement("method", [](const char* string) {
		std::cout << string << std::endl;
	});

	my_service.implement("add", [](int a, int b) {
		return a + b;
	});

	auto my_client = app.request("service");

	auto remote_fn = my_client.proxy<void(const char*)>("method");
	auto remote_add = my_client.proxy<float(float, float)>("add");
	
	remote_fn("hello world!");

	std::cout << remote_add(3, 5).get();
}