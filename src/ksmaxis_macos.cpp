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

		// Mouse-specific usages
		constexpr std::uint32_t kUsagePageButton = 0x09;

		// Wrap-around detection threshold (half of normalized range)
		constexpr double kWrapThreshold = 0.5;

		struct Device
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

		IOHIDManagerRef s_hidManager = nullptr;
		IOHIDManagerRef s_mouseHidManager = nullptr;
		std::vector<Device> s_devices;
		std::vector<MouseDevice> s_mouseDevices;
		bool s_initialized = false;
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

		Device* FindDevice(IOHIDDeviceRef deviceRef)
		{
			for (auto& dev : s_devices)
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

		void InputValueCallback(void* context, IOReturn result, void* sender, IOHIDValueRef valueRef)
		{
			if (!valueRef) return;

			IOHIDElementRef element = IOHIDValueGetElement(valueRef);
			if (!element) return;

			IOHIDDeviceRef deviceRef = IOHIDElementGetDevice(element);
			if (!deviceRef) return;

			Device* dev = FindDevice(deviceRef);
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

		void DeviceMatchedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef deviceRef)
		{
			if (!deviceRef) return;
			if (FindDevice(deviceRef)) return;

			Device dev{};
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

			s_devices.push_back(dev);

			IOHIDDeviceRegisterInputValueCallback(deviceRef, InputValueCallback, nullptr);
			IOHIDDeviceScheduleWithRunLoop(deviceRef, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
		}

		void DeviceRemovedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef deviceRef)
		{
			for (auto it = s_devices.begin(); it != s_devices.end(); ++it)
			{
				if (it->device == deviceRef)
				{
					s_devices.erase(it);
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

		s_hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
		if (!s_hidManager)
		{
			if (pErrorString)
			{
				*pErrorString = "IOHIDManagerCreate failed";
			}
			return false;
		}

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
				&kCFTypeDictionaryValueCallBacks
			);
			CFNumberRef pageRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usagePage);
			CFNumberRef usageRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usage);
			CFDictionarySetValue(matchDict, CFSTR(kIOHIDDeviceUsagePageKey), pageRef);
			CFDictionarySetValue(matchDict, CFSTR(kIOHIDDeviceUsageKey), usageRef);
			CFRelease(pageRef);
			CFRelease(usageRef);
			CFArrayAppendValue(matchArray, matchDict);
			CFRelease(matchDict);
		}

		IOHIDManagerSetDeviceMatchingMultiple(s_hidManager, matchArray);
		CFRelease(matchArray);

		IOHIDManagerRegisterDeviceMatchingCallback(s_hidManager, DeviceMatchedCallback, nullptr);
		IOHIDManagerRegisterDeviceRemovalCallback(s_hidManager, DeviceRemovedCallback, nullptr);
		IOHIDManagerRegisterInputValueCallback(s_hidManager, InputValueCallback, nullptr);

		IOHIDManagerScheduleWithRunLoop(s_hidManager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

		IOReturn openResult = IOHIDManagerOpen(s_hidManager, kIOHIDOptionsTypeNone);
		if (openResult != kIOReturnSuccess && openResult != kIOReturnExclusiveAccess)
		{
			if (pErrorString)
			{
				*pErrorString = GetIOReturnErrorString(openResult);
			}
			CFRelease(s_hidManager);
			s_hidManager = nullptr;
			return false;
		}

		for (int i = 0; i < 10; ++i)
		{
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
		}

		// Initialize mouse HID manager
		s_mouseHidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
		if (s_mouseHidManager)
		{
			std::int32_t mouseUsagePage = kHIDPage_GenericDesktop;
			std::int32_t mouseUsage = kHIDUsage_GD_Mouse;

			CFMutableDictionaryRef mouseMatchDict = CFDictionaryCreateMutable(
				kCFAllocatorDefault, 0,
				&kCFTypeDictionaryKeyCallBacks,
				&kCFTypeDictionaryValueCallBacks
			);
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

			for (int i = 0; i < 10; ++i)
			{
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
			}
		}

		s_initialized = true;
		s_firstUpdate = true;
		return true;
	}

	void Terminate()
	{
		if (s_hidManager)
		{
			IOHIDManagerUnscheduleFromRunLoop(s_hidManager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
			IOHIDManagerClose(s_hidManager, kIOHIDOptionsTypeNone);
			CFRelease(s_hidManager);
			s_hidManager = nullptr;
		}
		if (s_mouseHidManager)
		{
			IOHIDManagerUnscheduleFromRunLoop(s_mouseHidManager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
			IOHIDManagerClose(s_mouseHidManager, kIOHIDOptionsTypeNone);
			CFRelease(s_mouseHidManager);
			s_mouseHidManager = nullptr;
		}
		s_devices.clear();
		s_mouseDevices.clear();
		s_initialized = false;
	}

	void Update()
	{
		s_deltaAnalogStick = { 0.0, 0.0 };
		s_deltaSlider = { 0.0, 0.0 };
		s_deltaMouse = { 0.0, 0.0 };

		if (!s_initialized) return;

		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);

		for (auto& dev : s_devices)
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
		return s_deltaSlider;
	}
}

#endif
