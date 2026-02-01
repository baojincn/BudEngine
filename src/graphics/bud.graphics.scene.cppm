module;

#include <vector>

export module bud.graphics.scene;

export namespace bud::graphics {

	export class RenderMesh {};
	export class RenderMaterial {};
	export class RenderTexture {};
	export class RenderAnimation {};
	export class RenderLight {};
	export class RenderCamera {};
	export class RenderTransform {};
	export class RenderCollider {};
	export class RenderScript {};
	export class RenderComponent {};
	export class RenderEntity {};
	export class RenderInstance {};

	export class RenderScene {
	public:
		std::vector<RenderInstance> entities;
	};
}
