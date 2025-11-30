#ifdef __APPLE__

#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>

#include "ksmaxis/ksmaxis.hpp"

#include <vector>
#include <cstdio>

namespace ksmaxis
{
	namespace
	{
		constexpr std::uint32_t kUsagePageGenericDesktop = 0x01;
		constexpr std::uint32_t kUsageX = 0x30;
		constexpr std::uint32_t kUsageY = 0x31;
		constexpr std::uint32_t kUsageSlider = 0x36;
		constexpr std::uint32_t kUsageDial = 0x37;

		constexpr double kWrapThreshold = 0.5;
		constexpr double kDeviceMatchingWaitSec = 0.1;
		constexpr double kRunLoopIntervalSec = 0.01;

		struct JoystickDevice
		{
			IOHIDDeviceRef device = nullptr;
			char productName[256] = {};
			double axisX = 0.0;
			double axisY = 0.0;
			double slider0 = 0.0;
			double slider1 = 0.0;
			double prevAxisX = 0.0;
			double prevAxisY = 0.0;
			double prevSlider0 = 0.0;
			double prevSlider1 = 0.0;
		};

		struct MouseDevice
		{
			IOHIDDeviceRef device = nullptr;
			char productName[256] = {};
			double deltaX = 0.0;
			double deltaY = 0.0;
		};

		IOHIDManagerRef s_joystickHidManager = nullptr;
		IOHIDManagerRef s_mouseHidManager = nullptr;
		std::vector<JoystickDevice> s_joystickDevices;
		std::vector<MouseDevice> s_mouseDevices;
		DeviceFlags s_initializedDevices = DeviceFlags::None;
		bool s_firstUpdate = true;
		AxisValues s_deltaAnalogStick = { 0.0, 0.0 };
		AxisValues s_deltaSlider = { 0.0, 0.0 };
		AxisValues s_deltaMouse = { 0.0, 0.0 };

		const char* GetIOReturnErrorString(IOReturn result)
		{
			switch (result)
			{
			case kIOReturnSuccess:
				return "Success";
			case kIOReturnNotPermitted:
				return "Not permitted (Input Monitoring permission required)";
			case kIOReturnExclusiveAccess:
				return "Exclusive access (another process has seized the device)";
			default:
				return "Unknown error";
			}
		}

		// 0-255 -> 0.0-1.0
		double Normalize(std::int64_t value)
		{
			return static_cast<double>(value) / 255.0;
		}

		double CalculateDelta(double current, double prev)
		{
			double delta = current - prev;
			if (delta > kWrapThreshold) delta -= 1.0;
			else if (delta < -kWrapThreshold) delta += 1.0;
			return delta;
		}

		JoystickDevice* FindJoystickDevice(IOHIDDeviceRef deviceRef)
		{
			for (auto& dev : s_joystickDevices)
			{
				if (dev.device == deviceRef) return &dev;
			}
			return nullptr;
		}

		MouseDevice* FindMouseDevice(IOHIDDeviceRef deviceRef)
		{
			for (auto& dev : s_mouseDevices)
			{
				if (dev.device == deviceRef) return &dev;
			}
			return nullptr;
		}

		void JoystickInputValueCallback(void* context, IOReturn result, void* sender, IOHIDValueRef valueRef)
		{
			if (!valueRef) return;

			IOHIDElementRef element = IOHIDValueGetElement(valueRef);
			if (!element) return;

			IOHIDDeviceRef deviceRef = IOHIDElementGetDevice(element);
			if (!deviceRef) return;

			JoystickDevice* dev = FindJoystickDevice(deviceRef);
			if (!dev) return;

			std::uint32_t usagePage = IOHIDElementGetUsagePage(element);
			std::uint32_t usage = IOHIDElementGetUsage(element);

			if (usagePage != kUsagePageGenericDesktop) return;

			std::int64_t intValue = IOHIDValueGetIntegerValue(valueRef);
			double normalized = Normalize(intValue);

			if (usage == kUsageX)
			{
				dev->axisX = normalized;
			}
			else if (usage == kUsageY)
			{
				dev->axisY = normalized;
			}
			else if (usage == kUsageSlider)
			{
				dev->slider0 = normalized;
			}
			else if (usage == kUsageDial)
			{
				dev->slider1 = normalized;
			}
		}

		void JoystickDeviceMatchedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef deviceRef)
		{
			if (!deviceRef) return;
			if (FindJoystickDevice(deviceRef)) return;

			JoystickDevice dev{};
			dev.device = deviceRef;

			CFStringRef productRef = (CFStringRef)IOHIDDeviceGetProperty(deviceRef, CFSTR(kIOHIDProductKey));
			if (productRef)
			{
				CFStringGetCString(productRef, dev.productName, sizeof(dev.productName), kCFStringEncodingUTF8);
			}
			else
			{
				snprintf(dev.productName, sizeof(dev.productName), "Unknown Device");
			}

			s_joystickDevices.push_back(dev);

			IOHIDDeviceRegisterInputValueCallback(deviceRef, JoystickInputValueCallback, nullptr);
			IOHIDDeviceScheduleWithRunLoop(deviceRef, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
		}

		void JoystickDeviceRemovedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef deviceRef)
		{
			for (auto it = s_joystickDevices.begin(); it != s_joystickDevices.end(); ++it)
			{
				if (it->device == deviceRef)
				{
					s_joystickDevices.erase(it);
					break;
				}
			}
		}

		// Mouse callbacks
		void MouseInputValueCallback(void* context, IOReturn result, void* sender, IOHIDValueRef valueRef)
		{
			if (!valueRef) return;

			IOHIDElementRef element = IOHIDValueGetElement(valueRef);
			if (!element) return;

			IOHIDDeviceRef deviceRef = IOHIDElementGetDevice(element);
			if (!deviceRef) return;

			MouseDevice* dev = FindMouseDevice(deviceRef);
			if (!dev) return;

			std::uint32_t usagePage = IOHIDElementGetUsagePage(element);
			std::uint32_t usage = IOHIDElementGetUsage(element);

			if (usagePage != kUsagePageGenericDesktop) return;

			std::int64_t intValue = IOHIDValueGetIntegerValue(valueRef);

			if (usage == kUsageX)
			{
				dev->deltaX += static_cast<double>(intValue);
			}
			else if (usage == kUsageY)
			{
				dev->deltaY += static_cast<double>(intValue);
			}
		}

		void MouseDeviceMatchedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef deviceRef)
		{
			if (!deviceRef) return;
			if (FindMouseDevice(deviceRef)) return;

			MouseDevice dev{};
			dev.device = deviceRef;

			CFStringRef productRef = (CFStringRef)IOHIDDeviceGetProperty(deviceRef, CFSTR(kIOHIDProductKey));
			if (productRef)
			{
				CFStringGetCString(productRef, dev.productName, sizeof(dev.productName), kCFStringEncodingUTF8);
			}
			else
			{
				snprintf(dev.productName, sizeof(dev.productName), "Unknown Mouse");
			}

			s_mouseDevices.push_back(dev);

			IOHIDDeviceRegisterInputValueCallback(deviceRef, MouseInputValueCallback, nullptr);
			IOHIDDeviceScheduleWithRunLoop(deviceRef, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
		}

		void MouseDeviceRemovedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef deviceRef)
		{
			for (auto it = s_mouseDevices.begin(); it != s_mouseDevices.end(); ++it)
			{
				if (it->device == deviceRef)
				{
					s_mouseDevices.erase(it);
					break;
				}
			}
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

		// Initialize joystick HID manager (failure is non-fatal)
		if ((deviceFlags & DeviceFlags::Joystick) != DeviceFlags::None)
		{
			s_joystickHidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
			if (!s_joystickHidManager)
			{
				if (pWarningStrings)
				{
					pWarningStrings->push_back("Joystick IOHIDManagerCreate failed");
				}
			}
			else
			{
				std::int32_t usagePage = kHIDPage_GenericDesktop;
				std::int32_t usages[] = {
					kHIDUsage_GD_Joystick,
					kHIDUsage_GD_GamePad,
					kHIDUsage_GD_MultiAxisController
				};

				CFMutableArrayRef matchArray = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
				for (std::int32_t usage : usages)
				{
					CFMutableDictionaryRef matchDict = CFDictionaryCreateMutable(
						kCFAllocatorDefault, 0,
						&kCFTypeDictionaryKeyCallBacks,
						&kCFTypeDictionaryValueCallBacks);
					CFNumberRef pageRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usagePage);
					CFNumberRef usageRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usage);
					CFDictionarySetValue(matchDict, CFSTR(kIOHIDDeviceUsagePageKey), pageRef);
					CFDictionarySetValue(matchDict, CFSTR(kIOHIDDeviceUsageKey), usageRef);
					CFRelease(pageRef);
					CFRelease(usageRef);
					CFArrayAppendValue(matchArray, matchDict);
					CFRelease(matchDict);
				}

				IOHIDManagerSetDeviceMatchingMultiple(s_joystickHidManager, matchArray);
				CFRelease(matchArray);

				IOHIDManagerRegisterDeviceMatchingCallback(s_joystickHidManager, JoystickDeviceMatchedCallback, nullptr);
				IOHIDManagerRegisterDeviceRemovalCallback(s_joystickHidManager, JoystickDeviceRemovedCallback, nullptr);
				IOHIDManagerRegisterInputValueCallback(s_joystickHidManager, JoystickInputValueCallback, nullptr);

				IOHIDManagerScheduleWithRunLoop(s_joystickHidManager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

				IOReturn openResult = IOHIDManagerOpen(s_joystickHidManager, kIOHIDOptionsTypeNone);
				if (openResult != kIOReturnSuccess && openResult != kIOReturnExclusiveAccess)
				{
					if (pWarningStrings)
					{
						pWarningStrings->push_back(std::string{ "Joystick IOHIDManagerOpen failed: " } + GetIOReturnErrorString(openResult));
					}
					CFRelease(s_joystickHidManager);
					s_joystickHidManager = nullptr;
				}
				else
				{
					for (double t = 0.0; t < kDeviceMatchingWaitSec; t += kRunLoopIntervalSec)
					{
						CFRunLoopRunInMode(kCFRunLoopDefaultMode, kRunLoopIntervalSec, true);
					}
					s_initializedDevices = s_initializedDevices | DeviceFlags::Joystick;
				}
			}
		}

		// Initialize mouse HID manager
		if ((deviceFlags & DeviceFlags::Mouse) != DeviceFlags::None)
		{
			s_mouseHidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
			if (s_mouseHidManager)
			{
				std::int32_t mouseUsagePage = kHIDPage_GenericDesktop;
				std::int32_t mouseUsage = kHIDUsage_GD_Mouse;

				CFMutableDictionaryRef mouseMatchDict = CFDictionaryCreateMutable(
					kCFAllocatorDefault, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
				CFNumberRef mousePageRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &mouseUsagePage);
				CFNumberRef mouseUsageRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &mouseUsage);
				CFDictionarySetValue(mouseMatchDict, CFSTR(kIOHIDDeviceUsagePageKey), mousePageRef);
				CFDictionarySetValue(mouseMatchDict, CFSTR(kIOHIDDeviceUsageKey), mouseUsageRef);
				CFRelease(mousePageRef);
				CFRelease(mouseUsageRef);

				IOHIDManagerSetDeviceMatching(s_mouseHidManager, mouseMatchDict);
				CFRelease(mouseMatchDict);

				IOHIDManagerRegisterDeviceMatchingCallback(s_mouseHidManager, MouseDeviceMatchedCallback, nullptr);
				IOHIDManagerRegisterDeviceRemovalCallback(s_mouseHidManager, MouseDeviceRemovedCallback, nullptr);
				IOHIDManagerRegisterInputValueCallback(s_mouseHidManager, MouseInputValueCallback, nullptr);

				IOHIDManagerScheduleWithRunLoop(s_mouseHidManager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

				IOReturn mouseOpenResult = IOHIDManagerOpen(s_mouseHidManager, kIOHIDOptionsTypeNone);
				if (mouseOpenResult != kIOReturnSuccess && mouseOpenResult != kIOReturnExclusiveAccess)
				{
					if (pWarningStrings)
					{
						pWarningStrings->push_back(std::string{ "Mouse IOHIDManagerOpen failed: " } + GetIOReturnErrorString(mouseOpenResult));
					}
					CFRelease(s_mouseHidManager);
					s_mouseHidManager = nullptr;
				}
				else
				{
					for (double t = 0.0; t < kDeviceMatchingWaitSec; t += kRunLoopIntervalSec)
					{
						CFRunLoopRunInMode(kCFRunLoopDefaultMode, kRunLoopIntervalSec, true);
					}
					s_initializedDevices = s_initializedDevices | DeviceFlags::Mouse;
				}
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
		if (s_joystickHidManager)
		{
			IOHIDManagerUnscheduleFromRunLoop(s_joystickHidManager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
			IOHIDManagerClose(s_joystickHidManager, kIOHIDOptionsTypeNone);
			CFRelease(s_joystickHidManager);
			s_joystickHidManager = nullptr;
		}
		if (s_mouseHidManager)
		{
			IOHIDManagerUnscheduleFromRunLoop(s_mouseHidManager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
			IOHIDManagerClose(s_mouseHidManager, kIOHIDOptionsTypeNone);
			CFRelease(s_mouseHidManager);
			s_mouseHidManager = nullptr;
		}
		s_joystickDevices.clear();
		s_mouseDevices.clear();
		s_initializedDevices = DeviceFlags::None;
	}

	void Update()
	{
		s_deltaAnalogStick = { 0.0, 0.0 };
		s_deltaSlider = { 0.0, 0.0 };
		s_deltaMouse = { 0.0, 0.0 };

		if (s_initializedDevices == DeviceFlags::None) return;

		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);

		for (auto& dev : s_joystickDevices)
		{
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

		for (auto& dev : s_mouseDevices)
		{
			s_deltaMouse[0] += dev.deltaX;
			s_deltaMouse[1] += dev.deltaY;
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
		else
		{
			return s_deltaSlider;
		}
	}
}

#endif
