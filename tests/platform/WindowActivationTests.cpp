#include <doctest/doctest.h>

#include "platform/WindowActivation.hpp"

#ifdef _WIN32
TEST_CASE("Agent window activation policy preserves normal windows and blocks implicit activation")
{
	const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Hidden activation policy test",
		WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	REQUIRE(window != nullptr);
	CHECK(aether::ApplyAgentWindowActivationPolicy(window, false));
	CHECK(GetWindowLongPtrW(window, GWL_EXSTYLE) == WS_EX_TOOLWINDOW);
	CHECK(aether::ApplyAgentWindowActivationPolicy(window, true));
	CHECK(GetWindowLongPtrW(window, GWL_EXSTYLE) == (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE));
	CHECK(aether::ApplyAgentWindowActivationPolicy(window, true));
	CHECK_FALSE(aether::ApplyAgentWindowActivationPolicy(nullptr, true));
	CHECK(DestroyWindow(window));
}
#endif
