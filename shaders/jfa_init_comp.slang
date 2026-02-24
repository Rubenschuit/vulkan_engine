// JFA initialization: reads binary mask, writes seed coordinates to JFA texture.
// Seed pixels (mask > 0.5) get their own coordinate; non-seeds get sentinel (-1, -1).

[vk::binding(0, 0)]
Texture2D<float> mask_texture;
[vk::binding(1, 0)]
RWTexture2D<int2> jfa_out;

struct JfaInitPush {
	int2 image_size;
};
[[vk::push_constant]] JfaInitPush push;

[numthreads(16, 16, 1)]
[shader("compute")]
void compMain(uint3 dtid : SV_DispatchThreadID) {
	int2 coord = int2(dtid.xy);
	if (coord.x >= push.image_size.x || coord.y >= push.image_size.y)
		return;

	float mask = mask_texture.Load(int3(coord, 0));
	if (mask > 0.5)
		jfa_out[coord] = coord;
	else
		jfa_out[coord] = int2(-1, -1);
}
