#include <doctest/doctest.h>

#include "imgui/UiAutomation.hpp"
#include <imgui.h>
#include <imgui_internal.h>

using namespace aether::app;

TEST_CASE("UiAutomation FindItem resolves by window+label and computes centre")
{
    UiAutomation& a = UiAutomation::Get();
    a.RecordItemAdd(1u, 10.0f, 20.0f, 100.0f, 40.0f, "Lighting");
    a.RecordItemInfo(1u, "Light gizmos");
    a.RecordItemAdd(2u, 0.0f, 0.0f, 50.0f, 50.0f, "Other");
    a.RecordItemInfo(2u, "Light gizmos");
    a.BeginFrameSwap(); // publish building -> snapshot

    std::string err;
    const auto hit = a.FindItem("Lighting", "Light gizmos", err);
    REQUIRE(hit.has_value());
    CHECK(hit->x + hit->w / 2.0f == doctest::Approx(60.0f));
    CHECK(hit->y + hit->h / 2.0f == doctest::Approx(40.0f));

    const auto ambiguous = a.FindItem("", "Light gizmos", err);
    CHECK_FALSE(ambiguous.has_value());
    CHECK_FALSE(err.empty());

    const auto missing = a.FindItem("Lighting", "Nope", err);
    CHECK_FALSE(missing.has_value());

    a.BeginFrameSwap(); // clear so this test leaves no residue for others
}

TEST_CASE("Synthetic clicks route to detached viewports while the OS reports another viewport and focus loss")
{
	for (const float x : {-1470.0f, 470.0f})
	{
		ImGuiContext* gui = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.BackendFlags |= ImGuiBackendFlags_HasMouseHoveredViewport;
		ImGuiViewportP detached;
		detached.ID = 700;
		ImGuiWindow* window = IM_NEW(ImGuiWindow)(gui, "Detached recovery");
		gui->Windows.push_back(window);
		window->Viewport = &detached;
		window->ViewportId = detached.ID;
		window->Active = true;
		window->OuterRectClipped = ImRect(x - 20, 1000, x + 20, 1120);
		UiAutomation& automation = UiAutomation::Get();
		automation.Input().ClearHover();
		automation.Input().QueueClick(x, 1093.5f, 0, false);
		int frame = 0;
		while (automation.Input().Busy())
		{
			io.AddMouseViewportEvent(ImGui::GetMainViewport()->ID);
			io.AddFocusEvent(false);
			automation.ApplyInput(io);
			ImGui::UpdateInputEvents(false);
			CHECK(io.MouseHoveredViewport == detached.ID);
			CHECK_FALSE(io.AppFocusLost);
			CHECK(io.MousePos.x == x);
			CHECK(io.MouseDown[0] == (frame == 3));
			++frame;
		}
		CHECK(frame == 5);
		window->Viewport = gui->Viewports[0];
		ImGui::DestroyContext(gui);
	}
}

TEST_CASE("Deferring a modal can cancel queued UI input without a click-through or held modifiers")
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	auto& automation = UiAutomation::Get();
	automation.Input().QueueClick(30, 40, 0, true, true);
	automation.Input().QueueHover(30, 40);
	io.MouseDown[0] = true;
	io.KeyCtrl = true;
	io.KeyMods = ImGuiMod_Ctrl;
	automation.CancelInput();
	CHECK_FALSE(automation.Input().Busy());
	CHECK(automation.Input().Step().empty());
	CHECK_FALSE(io.MouseDown[0]);
	CHECK_FALSE(io.KeyCtrl);
	CHECK(io.KeyMods == 0);
	automation.ApplyInput(io);
	CHECK(ImGui::GetCurrentContext()->InputEventsQueue.empty());
	ImGui::DestroyContext();
}

TEST_CASE("Collapsed hierarchy child visibility skips tree work and recovers on reopening")
{
	ImGuiContext* gui = ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(800, 600);
	io.DeltaTime = 1.0f / 60.0f;
	unsigned char* pixels = nullptr;
	int width = 0, height = 0;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	for (const bool collapsed : {false, true, false})
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_Always);
		ImGui::SetNextWindowCollapsed(collapsed, ImGuiCond_Always);
		ImGui::Begin("Hierarchy visibility test");
		const bool contents = ImGui::BeginChild("SceneList", ImVec2(0, 0), ImGuiChildFlags_Borders,
			ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus);
		CHECK(contents == !collapsed);
		ImGui::EndChild();
		ImGui::End();
		ImGui::EndFrame();
	}
	ImGui::DestroyContext(gui);
}
