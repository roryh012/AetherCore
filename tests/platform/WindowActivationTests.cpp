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
	const HWND foreground = GetForegroundWindow();
	const HWND previous = GetWindow(window, GW_HWNDPREV);
	RECT before{};
	REQUIRE(GetWindowRect(window, &before));
	bool blocked = false;
	CHECK(aether::ReadOwnedWindowActivationBlock(window, blocked));
	CHECK(blocked);
	CHECK(aether::ReleaseOwnedWindowActivationBlock(window));
	CHECK(GetWindowLongPtrW(window, GWL_EXSTYLE) == WS_EX_TOOLWINDOW);
	CHECK(aether::ReadOwnedWindowActivationBlock(window, blocked));
	CHECK_FALSE(blocked);
	CHECK(aether::ReleaseOwnedWindowActivationBlock(window));
	RECT after{};
	REQUIRE(GetWindowRect(window, &after));
	CHECK(before.left == after.left);
	CHECK(before.top == after.top);
	CHECK(before.right == after.right);
	CHECK(before.bottom == after.bottom);
	CHECK(GetForegroundWindow() == foreground);
	CHECK(GetWindow(window, GW_HWNDPREV) == previous);
	CHECK_FALSE(IsWindowVisible(window));
	CHECK_FALSE(aether::ReadOwnedWindowActivationBlock(nullptr, blocked));
	CHECK_FALSE(aether::ReleaseOwnedWindowActivationBlock(nullptr));
	CHECK_FALSE(aether::ReleaseOwnedWindowActivationBlock(GetDesktopWindow()));
	CHECK_FALSE(aether::ApplyAgentWindowActivationPolicy(nullptr, true));
	CHECK(DestroyWindow(window));
}
#endif
