#include <doctest/doctest.h>

#include <array>
#include <glm/gtc/matrix_transform.hpp>

#include "rendering/FrameConstants.hpp"

TEST_CASE("Frustum planes agree with Vulkan clip space for perspective orthographic and reversed depth")
{
	const auto view = glm::lookAtRH(glm::vec3(3, 4, 9), glm::vec3(1, -3, -7), glm::vec3(0, 1, 0));
	std::array projections{
		glm::perspectiveRH_ZO(glm::radians(63.0f), 1.5f, 0.25f, 100.0f),
		glm::orthoRH_ZO(-10.0f, 10.0f, -6.0f, 6.0f, 0.25f, 100.0f),
		glm::perspectiveRH_ZO(glm::radians(63.0f), 1.5f, 100.0f, 0.25f)
	};
	for (auto projection : projections)
	{
		projection[1][1] *= -1.0f;
		aether::FrameConstants frame;
		frame.viewProj = projection * view;
		frame.RefreshDerived();
		for (int x = -20; x <= 20; x += 5)
		for (int y = -12; y <= 12; y += 4)
		for (int z = -60; z <= 60; z += 10)
		{
			const glm::vec4 point(x + 0.137f, y + 0.273f, z + 0.419f, 1);
			const auto clip = frame.viewProj * point;
			const bool inClip = clip.w > 0 && clip.x >= -clip.w && clip.x <= clip.w
				&& clip.y >= -clip.w && clip.y <= clip.w && clip.z >= 0 && clip.z <= clip.w;
			bool inPlanes = true;
			for (const auto& plane : frame.frustumPlanes) inPlanes &= glm::dot(plane, point) >= 0;
			CAPTURE(x);
			CAPTURE(y);
			CAPTURE(z);
			CHECK(inPlanes == inClip);
		}
	}
}
