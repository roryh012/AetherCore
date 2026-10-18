#pragma once

#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <Windows.h>

namespace aether
{
	inline bool ReadOwnedWindowActivationBlock(HWND window, bool& blocked)
	{
		DWORD process = 0;
		const DWORD thread = GetWindowThreadProcessId(window, &process);
		if (process != GetCurrentProcessId() || thread != GetCurrentThreadId()) { return false; }
		SetLastError(ERROR_SUCCESS);
		const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
		if (style == 0 && GetLastError() != ERROR_SUCCESS) { return false; }
		blocked = (style & WS_EX_NOACTIVATE) != 0;
		return true;
	}

	inline bool ReleaseOwnedWindowActivationBlock(HWND window)
	{
		bool blocked = false;
		if (!ReadOwnedWindowActivationBlock(window, blocked)) { return false; }
		if (!blocked) { return true; }
		const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
		SetLastError(ERROR_SUCCESS);
		if (SetWindowLongPtrW(window, GWL_EXSTYLE, style & ~WS_EX_NOACTIVATE) == 0 && GetLastError() != ERROR_SUCCESS) { return false; }
		constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;
		if (!SetWindowPos(window, nullptr, 0, 0, 0, 0, flags) || !ReadOwnedWindowActivationBlock(window, blocked) || blocked)
		{
			SetWindowLongPtrW(window, GWL_EXSTYLE, style);
			SetWindowPos(window, nullptr, 0, 0, 0, 0, flags);
			return false;
		}
		return true;
	}

	inline bool ApplyAgentWindowActivationPolicy(HWND window, const bool agentSession)
	{
		if (!agentSession) return true;
		SetLastError(ERROR_SUCCESS);
		const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
		if (style == 0 && GetLastError() != ERROR_SUCCESS) return false;
		SetLastError(ERROR_SUCCESS);
		return SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_NOACTIVATE) != 0
			|| GetLastError() == ERROR_SUCCESS;
	}
} // namespace aether
#endif
