#ifdef _WIN32

#ifndef UNICODE
#define UNICODE
#endif

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <hidusage.h>
#include <dinput.h>

#include "ksmaxis/ksmaxis.hpp"

#include <vector>
#include <comdef.h>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace ksmaxis
{
	namespace
	{
		// Wrap-around detection threshold (half of normalized range)
		constexpr double kWrapThreshold = 0.5;

		struct JoystickDevice
		{
			DIDEVICEINSTANCEW instance{};
			LPDIRECTINPUTDEVICE8W device = nullptr;
			double axisX = 0.0;
			double axisY = 0.0;
			double slider0 = 0.0;
			double slider1 = 0.0;
			double prevAxisX = 0.0;
			double prevAxisY = 0.0;
			double prevSlider0 = 0.0;
			double prevSlider1 = 0.0;
			bool opened = false;
		};

		LPDIRECTINPUT8W s_directInput = nullptr;
		std::vector<JoystickDevice> s_joystickDevices;
		bool s_initialized = false;
		bool s_firstUpdate = true;
		AxisValues s_deltaAnalogStick = { 0.0, 0.0 };
		AxisValues s_deltaSlider = { 0.0, 0.0 };
		AxisValues s_deltaMouse = { 0.0, 0.0 };
		AxisValues s_mouseAccumulator = { 0.0, 0.0 };

		HWND s_hiddenWnd = nullptr;
		ATOM s_windowClass = 0;

		constexpr wchar_t kWindowClassName[] = L"ksmaxis_RawInputWindow";

		LRESULT CALLBACK RawInputWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
		{
			if (msg == WM_INPUT)
			{
				UINT size = 0;
				GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));

				if (size > 0)
				{
					std::vector<BYTE> buffer(size);
					if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) == size)
					{
						RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
						if (raw->header.dwType == RIM_TYPEMOUSE)
						{
							// Only handle relative mouse movement
							if ((raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
							{
								s_mouseAccumulator[0] += static_cast<double>(raw->data.mouse.lLastX);
								s_mouseAccumulator[1] += static_cast<double>(raw->data.mouse.lLastY);
							}
						}
					}
				}
				return 0;
			}
			return DefWindowProcW(hWnd, msg, wParam, lParam);
		}

		double Normalize(LONG value)
		{
			// -32768~32767 -> 0.0~1.0
			return (static_cast<double>(value) + 32768.0) / 65535.0;
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

		BOOL CALLBACK EnumDevicesCallback(const DIDEVICEINSTANCEW* instance, VOID* context)
		{
			(void)context;
			JoystickDevice dev{};
			dev.instance = *instance;
			s_joystickDevices.push_back(dev);
			return DIENUM_CONTINUE;
		}

		std::string GetHResultErrorString(HRESULT hr)
		{
			_com_error err(hr);
			const wchar_t* msg = err.ErrorMessage();
			int size = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
			if (size <= 0)
			{
				return "Unknown error";
			}
			std::string result(size - 1, '\0');
			WideCharToMultiByte(CP_UTF8, 0, msg, -1, &result[0], size, nullptr, nullptr);
			return result;
		}

		bool InitRawInputWindow(std::string* pErrorString)
		{
			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(WNDCLASSEXW);
			wc.lpfnWndProc = RawInputWndProc;
			wc.hInstance = GetModuleHandle(nullptr);
			wc.lpszClassName = kWindowClassName;

			s_windowClass = RegisterClassExW(&wc);
			if (!s_windowClass)
			{
				DWORD err = GetLastError();
				if (err != ERROR_CLASS_ALREADY_EXISTS)
				{
					if (pErrorString)
					{
						*pErrorString = "Failed to register window class for raw input";
					}
					return false;
				}
			}

			// Message-only window
			s_hiddenWnd = CreateWindowExW(
				0,
				kWindowClassName,
				L"",
				0,
				0, 0, 0, 0,
				HWND_MESSAGE,
				nullptr,
				GetModuleHandle(nullptr),
				nullptr
			);

			if (!s_hiddenWnd)
			{
				if (pErrorString)
				{
					*pErrorString = "Failed to create hidden window for raw input";
				}
				return false;
			}

			RAWINPUTDEVICE rid{};
			rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
			rid.usUsage = HID_USAGE_GENERIC_MOUSE;
			rid.dwFlags = RIDEV_INPUTSINK;
			rid.hwndTarget = s_hiddenWnd;

			if (!RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE)))
			{
				if (pErrorString)
				{
					*pErrorString = "Failed to register raw input device";
				}
				DestroyWindow(s_hiddenWnd);
				s_hiddenWnd = nullptr;
				return false;
			}

			return true;
		}

		void TerminateRawInputWindow()
		{
			if (s_hiddenWnd)
			{
				DestroyWindow(s_hiddenWnd);
				s_hiddenWnd = nullptr;
			}

			if (s_windowClass)
			{
				UnregisterClassW(kWindowClassName, GetModuleHandle(nullptr));
				s_windowClass = 0;
			}
		}
	}

	bool Init(void* hWnd, std::string* pErrorString, std::vector<std::string>* pWarningStrings)
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

		// Initialize DirectInput for joysticks (failure is non-fatal)
		HRESULT hr = DirectInput8Create(
			GetModuleHandle(nullptr),
			DIRECTINPUT_VERSION,
			IID_IDirectInput8W,
			reinterpret_cast<void**>(&s_directInput),
			nullptr
		);

		if (FAILED(hr))
		{
			if (pWarningStrings)
			{
				pWarningStrings->push_back(std::string{ "DirectInput8Create failed: " } + GetHResultErrorString(hr));
			}
			s_directInput = nullptr;
		}

		if (s_directInput)
		{
			hr = s_directInput->EnumDevices(
				DI8DEVCLASS_GAMECTRL,
				EnumDevicesCallback,
				nullptr,
				DIEDFL_ATTACHEDONLY
			);

			if (FAILED(hr))
			{
				if (pWarningStrings)
				{
					pWarningStrings->push_back(std::string{ "EnumDevices failed: " } + GetHResultErrorString(hr));
				}
				s_directInput->Release();
				s_directInput = nullptr;
			}
		}

		// Open all joystick devices
		if (s_directInput)
		{
			for (auto& dev : s_joystickDevices)
			{
				hr = s_directInput->CreateDevice(dev.instance.guidInstance, &dev.device, nullptr);
				if (FAILED(hr))
				{
					continue;
				}

				hr = dev.device->SetDataFormat(&c_dfDIJoystick2);
				if (FAILED(hr))
				{
					dev.device->Release();
					dev.device = nullptr;
					continue;
				}

				HWND hwndForDInput = static_cast<HWND>(hWnd);
				if (!hwndForDInput)
				{
					hwndForDInput = GetConsoleWindow();
				}
				if (!hwndForDInput)
				{
					hwndForDInput = GetDesktopWindow();
				}

				hr = dev.device->SetCooperativeLevel(hwndForDInput, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
				if (FAILED(hr))
				{
					dev.device->Release();
					dev.device = nullptr;
					continue;
				}

				DIPROPRANGE propRange{};
				propRange.diph.dwSize = sizeof(DIPROPRANGE);
				propRange.diph.dwHeaderSize = sizeof(DIPROPHEADER);
				propRange.diph.dwHow = DIPH_BYOFFSET;
				propRange.lMin = -32768;
				propRange.lMax = 32767;

				DWORD offsets[] = { DIJOFS_X, DIJOFS_Y, DIJOFS_SLIDER(0), DIJOFS_SLIDER(1) };
				for (DWORD offset : offsets)
				{
					propRange.diph.dwObj = offset;
					dev.device->SetProperty(DIPROP_RANGE, &propRange.diph);
				}

				dev.device->Acquire();
				dev.opened = true;
			}
		}

		// Mouse input unavailable on failure, but continue for other devices
		std::string mouseError;
		if (!InitRawInputWindow(&mouseError) && pWarningStrings)
		{
			pWarningStrings->push_back(mouseError);
		}

		s_firstUpdate = true;
		return true;
	}

	void Terminate()
	{
		TerminateRawInputWindow();

		for (auto& dev : s_joystickDevices)
		{
			if (dev.device)
			{
				dev.device->Unacquire();
				dev.device->Release();
				dev.device = nullptr;
			}
		}
		s_joystickDevices.clear();

		if (s_directInput)
		{
			s_directInput->Release();
			s_directInput = nullptr;
		}

		s_initialized = false;
	}

	void Update()
	{
		s_deltaAnalogStick = { 0.0, 0.0 };
		s_deltaSlider = { 0.0, 0.0 };

		// Pump messages to process Raw Input
		if (s_hiddenWnd)
		{
			MSG msg;
			while (PeekMessageW(&msg, s_hiddenWnd, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
		}

		s_deltaMouse = s_mouseAccumulator;
		s_mouseAccumulator = { 0.0, 0.0 };

		if (!s_initialized)
		{
			return;
		}

		for (auto& dev : s_joystickDevices)
		{
			if (!dev.opened || !dev.device)
			{
				continue;
			}

			HRESULT hr = dev.device->Poll();
			if (FAILED(hr))
			{
				hr = dev.device->Acquire();
				if (FAILED(hr))
				{
					continue;
				}
				dev.device->Poll();
			}

			DIJOYSTATE2 js{};
			hr = dev.device->GetDeviceState(sizeof(DIJOYSTATE2), &js);
			if (FAILED(hr))
			{
				continue;
			}

			dev.axisX = Normalize(js.lX);
			dev.axisY = Normalize(js.lY);
			dev.slider0 = Normalize(js.rglSlider[1]); // Intentionally swapped ([0]=right knob, [1]=left knob)
			dev.slider1 = Normalize(js.rglSlider[0]);

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
