#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

#include "ksmaxis/ksmaxis.hpp"

int main()
{
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif

	std::atexit([]
	{
		std::cout << std::endl;
		ksmaxis::Terminate();
	});

	std::string errorString;
	if (!ksmaxis::Init(&errorString))
	{
		std::cerr << "Init failed: " << errorString << std::endl;
		return 1;
	}

	std::cout << "Reading all devices (Ctrl+C to exit)\n" << std::endl;

	while (true)
	{
		ksmaxis::Update();

		auto stick = ksmaxis::GetAxisDeltas(ksmaxis::InputMode::kAnalogStick);
		auto slider = ksmaxis::GetAxisDeltas(ksmaxis::InputMode::kSlider);

		std::cout << "\rStick: X=" << std::setw(6) << std::fixed << std::setprecision(2) << stick[0]
		          << " Y=" << std::setw(6) << stick[1]
		          << " | Slider: 0=" << std::setw(6) << slider[0]
		          << " 1=" << std::setw(6) << slider[1]
		          << std::flush;

		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}

	return 0;
}
