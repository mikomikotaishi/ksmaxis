#pragma once
#include <cstdint>
#include <array>
#include <string>

namespace ksmaxis
{
	enum class InputMode : std::uint8_t
	{
		kAnalogStick, // X/Y
		kSlider, // Slider0/Slider1
	};

	using AxisValues = std::array<double, 2>;

	bool Init(std::string* pErrorString = nullptr);

	void Terminate();

	void Update();

	[[nodiscard]]
	AxisValues GetAxisDeltas(InputMode mode);
}
