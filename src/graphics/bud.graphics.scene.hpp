#pragma once

#include <vector>

namespace bud::graphics {

	 class RenderMesh {};
	 class RenderMaterial {};
	 class RenderTexture {};
	 class RenderAnimation {};
	 class RenderLight {};
	 class RenderCamera {};
	 class RenderTransform {};
	 class RenderCollider {};
	 class RenderScript {};
	 class RenderComponent {};
	 class RenderEntity {};
	 class RenderInstance {};

	 class RenderScene {
	public:
		std::vector<RenderInstance> entities;
	};
}
