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

	enum class DeviceFlags : std::uint32_t
	{
		None = 0,
		Joystick = 1 << 0,
		Mouse = 1 << 1,
		All = Joystick | Mouse,
	};

	constexpr DeviceFlags operator|(DeviceFlags a, DeviceFlags b) noexcept
	{
		return static_cast<DeviceFlags>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
	}

	constexpr DeviceFlags operator&(DeviceFlags a, DeviceFlags b) noexcept
	{
		return static_cast<DeviceFlags>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
	}

	constexpr DeviceFlags operator~(DeviceFlags a) noexcept
	{
		return static_cast<DeviceFlags>(~static_cast<std::uint32_t>(a));
	}

	constexpr DeviceFlags& operator|=(DeviceFlags& a, DeviceFlags b) noexcept
	{
		return a = a | b;
	}

	constexpr DeviceFlags& operator&=(DeviceFlags& a, DeviceFlags b) noexcept
	{
		return a = a & b;
	}

	using AxisValues = std::array<double, 2>;

#ifdef _WIN32
	bool Init(DeviceFlags deviceFlags, void* hWnd, std::string* pErrorString = nullptr, std::vector<std::string>* pWarningStrings = nullptr);
#else
	bool Init(DeviceFlags deviceFlags, std::string* pErrorString = nullptr, std::vector<std::string>* pWarningStrings = nullptr);
#endif

	void Terminate();

	[[nodiscard]]
	bool IsInitialized();

	[[nodiscard]]
	bool IsInitialized(DeviceFlags deviceFlags);

	void Update();

	[[nodiscard]]
	AxisValues GetAxisDeltas(InputMode mode);

	[[nodiscard]]
	constexpr DeviceFlags GetRequiredDeviceFlags(InputMode mode) noexcept
	{
		switch (mode)
		{
		case InputMode::kMouse:
			return DeviceFlags::Mouse;
		case InputMode::kAnalogStick:
		case InputMode::kSlider:
			return DeviceFlags::Joystick;
		default:
			return DeviceFlags::None;
		}
	}
}
