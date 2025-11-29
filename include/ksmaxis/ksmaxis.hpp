#pragma once
#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace ksmaxis
{
	enum class InputMode : std::uint8_t
	{
		kAnalogStick, // X/Y
		kSlider, // Slider0/Slider1
		kMouse, // Mouse X/Y
	};

	using AxisValues = std::array<double, 2>;

#ifdef _WIN32
	bool Init(void* hWnd, std::string* pErrorString = nullptr, std::vector<std::string>* pWarningStrings = nullptr);
#else
	bool Init(std::string* pErrorString = nullptr, std::vector<std::string>* pWarningStrings = nullptr);
#endif

	void Terminate();

	void Update();

	[[nodiscard]]
	AxisValues GetAxisDeltas(InputMode mode);
}
