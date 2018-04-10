#include "o2xx.h"

#include <iostream>

int main() {
	using namespace o2;
	application app{ "test" };

	// we implement methods by providing a o2 service
	auto my_service = app.provide("service");

	// the simplest way to expose plain function pointers as o2 methods.
	// method typestring will be inferred from the signature
	my_service.implement("puts", puts);

	// lambdas or other functor objects can also be used.
	my_service.implement("print-reverse", [](std::string text) {
		std::reverse(text.begin(), text.end());
		std::cout << text << std::endl;
	});

	// methods can even return values. they will be provided via
	// o2 reply methods and support type coercion in and out.
	my_service.implement("add", [](int a, int b) {
		return a + b;
	});

	// client represents a remote o2 service. here it is in-process,
	// but could equally well be remote.
	auto my_client = app.request("service");

	// create proxy objects that represent remote methods.
	// the provided signatures apply for the proxy object, and must
	// be compatible with the remote method. o2 type coercion is
	// enabled, so type match does not need to be exact.
	auto remote_puts = my_client.proxy<decltype(puts)>("puts");
	auto remote_reverse = my_client.proxy<void(const char*)>("print-reverse");
	auto remote_add = my_client.proxy<float(float, float)>("add");
	
	// call some remote methods
	remote_puts("hello world!");
	remote_reverse("hello again.");

	// remote method with return value returns a future
	auto result_future = remote_add(3, 5);

	// wait for the value and print
	std::cout << result_future.get();
}