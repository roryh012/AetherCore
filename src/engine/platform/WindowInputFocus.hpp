#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <Windows.h>
#	define GLFW_EXPOSE_NATIVE_WIN32
#	include <GLFW/glfw3native.h>
#endif

namespace aether
{
	inline bool IsWindowInputFocused(GLFWwindow* window)
	{
		if (window == nullptr || glfwGetWindowAttrib(window, GLFW_FOCUSED) == 0)
		{
			return false;
		}
#ifdef _WIN32
		// GLFW 3.4 uses GetActiveWindow: a thread-local active window can be in the background.
		const HWND native = glfwGetWin32Window(window);
		return native == GetForegroundWindow() && IsWindowVisible(native) && !IsIconic(native);
#else
		return true;
#endif
	}
}
