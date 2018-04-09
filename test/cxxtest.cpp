#include "o2xx.h"

#include <iostream>

int main() {
	using namespace o2;
	application app{ "test" };
	auto my_service = app.provide("service");

	my_service.implement("method", [](const char* string) {
		std::cout << string << std::endl;
	});

	auto my_client = app.request("service");
	my_client.send("method", 100, "hello world");
}