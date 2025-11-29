#ifdef __linux__

#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>

#include "ksmaxis/ksmaxis.hpp"

#include <vector>
#include <string>
#include <cstring>
#include <climits>
#include <chrono>
#include <cerrno>

namespace ksmaxis
{
	namespace
	{
		constexpr std::size_t kBitsPerLong = CHAR_BIT * sizeof(unsigned long);

		// Wrap-around detection threshold (half of normalized range)
		constexpr double kWrapThreshold = 0.5;

		struct AxisRange
		{
			std::int32_t min = 0;
			std::int32_t max = 255;
			bool available = false;
		};

		struct JoystickDevice
		{
			std::string path;
			int fd = -1;
			double axisX = 0.0;
			double axisY = 0.0;
			double slider0 = 0.0;
			double slider1 = 0.0;
			double prevAxisX = 0.0;
			double prevAxisY = 0.0;
			double prevSlider0 = 0.0;
			double prevSlider1 = 0.0;
			AxisRange ranges[ABS_CNT]{};
			bool opened = false;
		};

		struct MouseDevice
		{
			std::string path;
			int fd = -1;
			double deltaX = 0.0;
			double deltaY = 0.0;
			bool opened = false;
		};

		std::vector<JoystickDevice> s_joystickDevices;
		std::vector<MouseDevice> s_mouseDevices;
		bool s_initialized = false;
		bool s_firstUpdate = true;
		AxisValues s_deltaAnalogStick = { 0.0, 0.0 };
		AxisValues s_deltaSlider = { 0.0, 0.0 };
		AxisValues s_deltaMouse = { 0.0, 0.0 };
		std::chrono::steady_clock::time_point s_lastScanTime;

		double Normalize(const JoystickDevice& dev, std::int32_t code, std::int32_t value)
		{
			if (code < 0 || code >= ABS_CNT || !dev.ranges[code].available)
			{
				return 0.0;
			}

			std::int32_t min = dev.ranges[code].min;
			std::int32_t max = dev.ranges[code].max;

			if (max == min)
			{
				return 0.0;
			}

			// min~max -> 0.0~1.0
			return static_cast<double>(value - min) / static_cast<double>(max - min);
		}

		double CalculateDelta(double current, double prev)
		{
			double delta = current - prev;

			// Wrap-around correction
			if (delta > kWrapThreshold)
			{
				delta -= 1.0;
			}
			else if (delta < -kWrapThreshold)
			{
				delta += 1.0;
			}

			return delta;
		}

		bool IsJoystickDeviceAlreadyOpened(const std::string& path)
		{
			for (const auto& dev : s_joystickDevices)
			{
				if (dev.path == path)
				{
					return true;
				}
			}
			return false;
		}

		bool IsMouseDeviceAlreadyOpened(const std::string& path)
		{
			for (const auto& dev : s_mouseDevices)
			{
				if (dev.path == path)
				{
					return true;
				}
			}
			return false;
		}

		void RemoveDisconnectedJoystickDevices()
		{
			for (auto it = s_joystickDevices.begin(); it != s_joystickDevices.end();)
			{
				if (!it->opened || it->fd < 0)
				{
					++it;
					continue;
				}

				struct input_id id;
				if (ioctl(it->fd, EVIOCGID, &id) < 0)
				{
					close(it->fd);
					it = s_joystickDevices.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		void RemoveDisconnectedMouseDevices()
		{
			for (auto it = s_mouseDevices.begin(); it != s_mouseDevices.end();)
			{
				if (!it->opened || it->fd < 0)
				{
					++it;
					continue;
				}

				struct input_id id;
				if (ioctl(it->fd, EVIOCGID, &id) < 0)
				{
					close(it->fd);
					it = s_mouseDevices.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		void ScanJoystickDevices()
		{
			DIR* dir = opendir("/dev/input");
			if (!dir)
			{
				return;
			}

			struct dirent* entry;
			while ((entry = readdir(dir)) != nullptr)
			{
				if (strncmp(entry->d_name, "event", 5) != 0)
				{
					continue;
				}

				std::string path = "/dev/input/" + std::string{ entry->d_name };

				// Skip if already opened
				if (IsJoystickDeviceAlreadyOpened(path))
				{
					continue;
				}

				int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
				if (fd < 0)
				{
					continue;
				}

				unsigned long evBits[(EV_CNT + kBitsPerLong - 1) / kBitsPerLong] = {};
				if (ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits) < 0)
				{
					close(fd);
					continue;
				}

				bool hasAbs = evBits[EV_ABS / kBitsPerLong] & (1UL << (EV_ABS % kBitsPerLong));
				if (!hasAbs)
				{
					close(fd);
					continue;
				}

				JoystickDevice dev{};
				dev.path = path;
				dev.fd = fd;

				unsigned long absBits[(ABS_CNT + kBitsPerLong - 1) / kBitsPerLong] = {};
				if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) >= 0)
				{
					for (int i = 0; i < ABS_CNT; ++i)
					{
						if (absBits[i / kBitsPerLong] & (1UL << (i % kBitsPerLong)))
						{
							struct input_absinfo absInfo{};
							if (ioctl(fd, EVIOCGABS(i), &absInfo) >= 0)
							{
								dev.ranges[i].min = absInfo.minimum;
								dev.ranges[i].max = absInfo.maximum;
								dev.ranges[i].available = true;
							}
						}
					}
				}

				dev.opened = true;
				s_joystickDevices.push_back(std::move(dev));
			}

			closedir(dir);
		}

		void ScanMouseDevices(std::vector<std::string>* pWarningStrings = nullptr)
		{
			DIR* dir = opendir("/dev/input");
			if (!dir)
			{
				return;
			}

			bool permissionErrorReported = false;
			struct dirent* entry;
			while ((entry = readdir(dir)) != nullptr)
			{
				if (strncmp(entry->d_name, "event", 5) != 0)
				{
					continue;
				}

				std::string path = "/dev/input/" + std::string{ entry->d_name };

				// Skip if already opened as joystick
				if (IsJoystickDeviceAlreadyOpened(path))
				{
					continue;
				}

				// Skip if already opened as mouse
				if (IsMouseDeviceAlreadyOpened(path))
				{
					continue;
				}

				int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
				if (fd < 0)
				{
					if (errno == EACCES && !permissionErrorReported && pWarningStrings)
					{
						pWarningStrings->push_back("Permission denied: " + path + " (add user to 'input' group)");
						permissionErrorReported = true;
					}
					continue;
				}

				unsigned long evBits[(EV_CNT + kBitsPerLong - 1) / kBitsPerLong] = {};
				if (ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits) < 0)
				{
					close(fd);
					continue;
				}

				// Check for relative events (mice report EV_REL)
				bool hasRel = evBits[EV_REL / kBitsPerLong] & (1UL << (EV_REL % kBitsPerLong));
				if (!hasRel)
				{
					close(fd);
					continue;
				}

				// Check for REL_X and REL_Y (mouse movement axes)
				unsigned long relBits[(REL_CNT + kBitsPerLong - 1) / kBitsPerLong] = {};
				if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relBits)), relBits) < 0)
				{
					close(fd);
					continue;
				}

				bool hasRelX = relBits[REL_X / kBitsPerLong] & (1UL << (REL_X % kBitsPerLong));
				bool hasRelY = relBits[REL_Y / kBitsPerLong] & (1UL << (REL_Y % kBitsPerLong));
				if (!hasRelX || !hasRelY)
				{
					close(fd);
					continue;
				}

				MouseDevice dev{};
				dev.path = path;
				dev.fd = fd;
				dev.opened = true;
				s_mouseDevices.push_back(std::move(dev));
			}

			closedir(dir);
		}
	}

	bool Init(std::string* pErrorString, std::vector<std::string>* pWarningStrings)
	{
		if (s_initialized)
		{
			if (pErrorString)
			{
				*pErrorString = "Already initialized";
			}
			return false;
		}

		s_initialized = true;
		s_firstUpdate = true;
		s_lastScanTime = std::chrono::steady_clock::now();
		ScanJoystickDevices();
		ScanMouseDevices(pWarningStrings);

		return true;
	}

	void Terminate()
	{
		for (auto& dev : s_joystickDevices)
		{
			if (dev.fd >= 0)
			{
				close(dev.fd);
				dev.fd = -1;
			}
		}
		s_joystickDevices.clear();

		for (auto& dev : s_mouseDevices)
		{
			if (dev.fd >= 0)
			{
				close(dev.fd);
				dev.fd = -1;
			}
		}
		s_mouseDevices.clear();

		s_initialized = false;
	}

	void Update()
	{
		s_deltaAnalogStick = { 0.0, 0.0 };
		s_deltaSlider = { 0.0, 0.0 };
		s_deltaMouse = { 0.0, 0.0 };

		if (!s_initialized)
		{
			return;
		}

		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastScanTime);
		if (elapsed.count() >= 1000)
		{
			RemoveDisconnectedJoystickDevices();
			RemoveDisconnectedMouseDevices();
			ScanJoystickDevices();
			ScanMouseDevices();
			s_lastScanTime = now;
		}

		for (auto& dev : s_joystickDevices)
		{
			if (!dev.opened || dev.fd < 0)
			{
				continue;
			}

			struct input_event ev{};
			while (read(dev.fd, &ev, sizeof(ev)) == sizeof(ev))
			{
				if (ev.type != EV_ABS)
				{
					continue;
				}

				double normalized = Normalize(dev, ev.code, ev.value);

				switch (ev.code)
				{
				case ABS_X:
					dev.axisX = normalized;
					break;
				case ABS_Y:
					dev.axisY = normalized;
					break;
				case ABS_THROTTLE:
				case ABS_MISC:
					dev.slider0 = normalized;
					break;
				case ABS_RUDDER:
					dev.slider1 = normalized;
					break;
				}
			}

			if (!s_firstUpdate)
			{
				s_deltaAnalogStick[0] += CalculateDelta(dev.axisX, dev.prevAxisX);
				s_deltaAnalogStick[1] += CalculateDelta(dev.axisY, dev.prevAxisY);
				s_deltaSlider[0] += CalculateDelta(dev.slider0, dev.prevSlider0);
				s_deltaSlider[1] += CalculateDelta(dev.slider1, dev.prevSlider1);
			}

			dev.prevAxisX = dev.axisX;
			dev.prevAxisY = dev.axisY;
			dev.prevSlider0 = dev.slider0;
			dev.prevSlider1 = dev.slider1;
		}

		// Process mouse devices
		for (auto& dev : s_mouseDevices)
		{
			if (!dev.opened || dev.fd < 0)
			{
				continue;
			}

			struct input_event ev{};
			while (read(dev.fd, &ev, sizeof(ev)) == sizeof(ev))
			{
				if (ev.type != EV_REL)
				{
					continue;
				}

				if (ev.code == REL_X)
				{
					dev.deltaX += static_cast<double>(ev.value);
				}
				else if (ev.code == REL_Y)
				{
					dev.deltaY += static_cast<double>(ev.value);
				}
			}

			// Accumulate mouse deltas from all devices
			s_deltaMouse[0] += dev.deltaX;
			s_deltaMouse[1] += dev.deltaY;

			// Reset device accumulators
			dev.deltaX = 0.0;
			dev.deltaY = 0.0;
		}

		s_firstUpdate = false;
	}

	AxisValues GetAxisDeltas(InputMode mode)
	{
		if (mode == InputMode::kAnalogStick)
		{
			return s_deltaAnalogStick;
		}
		else if (mode == InputMode::kMouse)
		{
			return s_deltaMouse;
		}
		return s_deltaSlider;
	}
}

#endif
