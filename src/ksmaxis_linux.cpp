#ifdef __linux__

#include "ksmaxis/ksmaxis.hpp"

#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <vector>
#include <string>
#include <cstring>
#include <climits>
#include <chrono>
#include <cerrno>

#undef None

namespace ksmaxis
{
	namespace
	{
		constexpr std::size_t kBitsPerLong = CHAR_BIT * sizeof(unsigned long);
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

		struct X11MouseContext
		{
			Display* display = nullptr;
			int xiOpcode = -1;
			double deltaX = 0.0;
			double deltaY = 0.0;
			bool initialized = false;
		};

		std::vector<JoystickDevice> s_joystickDevices;
		X11MouseContext s_x11Mouse;
		DeviceFlags s_initializedDevices = DeviceFlags::None;
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

		bool InitX11Mouse(std::vector<std::string>* pWarningStrings)
		{
			s_x11Mouse.display = XOpenDisplay(nullptr);
			if (!s_x11Mouse.display)
			{
				if (pWarningStrings)
				{
					pWarningStrings->push_back("Failed to open X11 display");
				}
				return false;
			}

			int xiEvent, xiError;
			if (!XQueryExtension(s_x11Mouse.display, "XInputExtension", &s_x11Mouse.xiOpcode, &xiEvent, &xiError))
			{
				if (pWarningStrings)
				{
					pWarningStrings->push_back("XInput extension not available");
				}
				XCloseDisplay(s_x11Mouse.display);
				s_x11Mouse.display = nullptr;
				return false;
			}

			int major = 2;
			int minor = 2;
			if (XIQueryVersion(s_x11Mouse.display, &major, &minor) != Success)
			{
				if (pWarningStrings)
				{
					pWarningStrings->push_back("XInput2 version 2.2 not available");
				}
				XCloseDisplay(s_x11Mouse.display);
				s_x11Mouse.display = nullptr;
				return false;
			}

			XIEventMask eventMask;
			unsigned char maskData[XIMaskLen(XI_RawMotion)] = {};
			XISetMask(maskData, XI_RawMotion);

			eventMask.deviceid = XIAllMasterDevices;
			eventMask.mask_len = sizeof(maskData);
			eventMask.mask = maskData;

			Window root = DefaultRootWindow(s_x11Mouse.display);
			XISelectEvents(s_x11Mouse.display, root, &eventMask, 1);
			XFlush(s_x11Mouse.display);

			s_x11Mouse.initialized = true;
			return true;
		}

		void TerminateX11Mouse()
		{
			if (s_x11Mouse.display)
			{
				XCloseDisplay(s_x11Mouse.display);
				s_x11Mouse.display = nullptr;
			}
			s_x11Mouse.initialized = false;
			s_x11Mouse.xiOpcode = -1;
			s_x11Mouse.deltaX = 0.0;
			s_x11Mouse.deltaY = 0.0;
		}
	}

	bool Init(DeviceFlags deviceFlags, std::string* pErrorString, std::vector<std::string>* pWarningStrings)
	{
		// Skip already initialized devices
		deviceFlags = deviceFlags & ~s_initializedDevices;
		if (deviceFlags == DeviceFlags::None)
		{
			return true;
		}

		s_firstUpdate = true;
		s_lastScanTime = std::chrono::steady_clock::now();

		if ((deviceFlags & DeviceFlags::Joystick) != DeviceFlags::None)
		{
			ScanJoystickDevices();
			s_initializedDevices = s_initializedDevices | DeviceFlags::Joystick;
		}

		if ((deviceFlags & DeviceFlags::Mouse) != DeviceFlags::None)
		{
			if (InitX11Mouse(pWarningStrings))
			{
				s_initializedDevices = s_initializedDevices | DeviceFlags::Mouse;
			}
		}

		return true;
	}

	bool IsInitialized()
	{
		return s_initializedDevices != DeviceFlags::None;
	}

	bool IsInitialized(DeviceFlags deviceFlags)
	{
		return (s_initializedDevices & deviceFlags) == deviceFlags;
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

		TerminateX11Mouse();

		s_initializedDevices = DeviceFlags::None;
	}

	void Update()
	{
		s_deltaAnalogStick = { 0.0, 0.0 };
		s_deltaSlider = { 0.0, 0.0 };
		s_deltaMouse = { 0.0, 0.0 };

		if (s_initializedDevices == DeviceFlags::None)
		{
			return;
		}

		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastScanTime);
		if (elapsed.count() >= 1000)
		{
			RemoveDisconnectedJoystickDevices();
			ScanJoystickDevices();
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

		if (s_x11Mouse.initialized && s_x11Mouse.display)
		{
			s_x11Mouse.deltaX = 0.0;
			s_x11Mouse.deltaY = 0.0;

			while (XPending(s_x11Mouse.display) > 0)
			{
				XEvent event;
				XNextEvent(s_x11Mouse.display, &event);

				XGenericEventCookie* cookie = &event.xcookie;
				if (cookie->type == GenericEvent && cookie->extension == s_x11Mouse.xiOpcode && XGetEventData(s_x11Mouse.display, cookie))
				{
					if (cookie->evtype == XI_RawMotion)
					{
						XIRawEvent* rawEvent = reinterpret_cast<XIRawEvent*>(cookie->data);
						double* rawValues = rawEvent->raw_values;

						if (XIMaskIsSet(rawEvent->valuators.mask, 0))
						{
							s_x11Mouse.deltaX += *rawValues;
							rawValues++;
						}
						if (XIMaskIsSet(rawEvent->valuators.mask, 1))
						{
							s_x11Mouse.deltaY += *rawValues;
						}
					}
					XFreeEventData(s_x11Mouse.display, cookie);
				}
			}

			s_deltaMouse[0] = s_x11Mouse.deltaX;
			s_deltaMouse[1] = s_x11Mouse.deltaY;
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
		else
		{
			return s_deltaSlider;
		}
	}
}

#endif
