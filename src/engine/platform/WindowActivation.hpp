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
