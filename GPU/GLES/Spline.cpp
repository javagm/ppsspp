// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "TransformPipeline.h"
#include "Core/MemMap.h"

// Just to get something on the screen, we'll just not subdivide correctly.
void TransformDrawEngine::DrawBezier(int ucount, int vcount) {
	u16 indices[3 * 3 * 6];

	static bool reported = false;
	if (!reported) {
		Reporting::ReportMessage("Unsupported bezier curve");
		reported = true;
	}

	// if (gstate.patchprimitive)
	// Generate indices for a rectangular mesh.
	int c = 0;
	for (int y = 0; y < 3; y++) {
		for (int x = 0; x < 3; x++) {
			indices[c++] = y * 4 + x;
			indices[c++] = y * 4 + x + 1;
			indices[c++] = (y + 1) * 4 + x + 1;
			indices[c++] = (y + 1) * 4 + x + 1;
			indices[c++] = (y + 1) * 4 + x;
			indices[c++] = y * 4 + x;
		}
	}

	// We are free to use the "decoded" buffer here.
	// Let's split it into two to get a second buffer, there's enough space.
	u8 *decoded2 = decoded + 65536 * 24;

	// Alright, now for the vertex data.
	// For now, we will simply inject UVs.

	float customUV[4 * 4 * 2];
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			customUV[(y * 4 + x) * 2 + 0] = (float)x/3.0f;
			customUV[(y * 4 + x) * 2 + 1] = (float)y/3.0f;
		}
	}

	if (!gstate.getTexCoordMask()) {
		VertexDecoder *dec = GetVertexDecoder(gstate.vertType);
		dec->SetVertexType(gstate.vertType);
		u32 newVertType = dec->InjectUVs(decoded2, Memory::GetPointer(gstate_c.vertexAddr), customUV, 16);
		SubmitPrim(decoded2, &indices[0], GE_PRIM_TRIANGLES, c, newVertType, GE_VTYPE_IDX_16BIT, 0);
	} else {
		SubmitPrim(Memory::GetPointer(gstate_c.vertexAddr), &indices[0], GE_PRIM_TRIANGLES, c, gstate.vertType, GE_VTYPE_IDX_16BIT, 0);
	}
	Flush();  // as our vertex storage here is temporary, it will only survive one draw.
}


// Spline implementation copied and modified from neobrain's softgpu (orphis code?)

#define START_OPEN_U 1
#define END_OPEN_U 2
#define START_OPEN_V 4
#define END_OPEN_V 8

// We decode all vertices into a common format for easy interpolation and stuff.
// Not fast but this spline stuff is rarely used for time critical things, strangely enough.
struct HWSplinePatch {
	u8 *points[16];
	int type;
};

void CopyTriangle(u8 *&dest, u8 *v1, u8 *v2, u8 * v3, int vertexSize) {
	memcpy(dest, v1, vertexSize);
	dest += vertexSize;
	memcpy(dest, v2, vertexSize);
	dest += vertexSize;
	memcpy(dest, v3, vertexSize);
	dest += vertexSize;
}

void TransformDrawEngine::SubmitSpline(void* control_points, void* indices, int count_u, int count_v, int type_u, int type_v, GEPatchPrimType prim_type, u32 vertex_type)
{
	Flush();

	if (prim_type != GE_PATCHPRIM_TRIANGLES) {
		// Only triangles supported!
		return;
	}

	// We're not actually going to decode, only reshuffle.
	VertexDecoder vdecoder;
	vdecoder.SetVertexType(vertex_type);

	int undecodedVertexSize = vdecoder.VertexSize();

	const DecVtxFormat& vtxfmt = vdecoder.GetDecVtxFmt();

	u16 index_lower_bound = 0;
	u16 index_upper_bound = count_u * count_v - 1;
	bool indices_16bit = (vertex_type & GE_VTYPE_IDX_MASK) == GE_VTYPE_IDX_16BIT;
	u8* indices8 = (u8*)indices;
	u16* indices16 = (u16*)indices;
	if (indices)
		GetIndexBounds(indices, count_u*count_v, vertex_type, &index_lower_bound, &index_upper_bound);

	int num_patches_u = count_u - 3;
	int num_patches_v = count_v - 3;

	// TODO: Do something less idiotic to manage this buffer
	HWSplinePatch* patches = new HWSplinePatch[num_patches_u * num_patches_v];
	for (int patch_u = 0; patch_u < num_patches_u; ++patch_u) {
		for (int patch_v = 0; patch_v < num_patches_v; ++patch_v) {
			HWSplinePatch& patch = patches[patch_u + patch_v * num_patches_u];

			for (int point = 0; point < 16; ++point) {
				int idx = (patch_u + point%4) + (patch_v + point/4) * count_u;
				if (indices)
					patch.points[point] = (u8 *)control_points + undecodedVertexSize * (indices_16bit ? indices16[idx] : indices8[idx]);
				else
					patch.points[point] = (u8 *)control_points + undecodedVertexSize * idx;
			}
			patch.type = (type_u | (type_v<<2));
			if (patch_u != 0) patch.type &= ~START_OPEN_U;
			if (patch_v != 0) patch.type &= ~START_OPEN_V;
			if (patch_u != num_patches_u-1) patch.type &= ~END_OPEN_U;
			if (patch_v != num_patches_v-1) patch.type &= ~END_OPEN_V;
		}
	}

	u8 *decoded2 = decoded + 65536 * 24;

	int count = 0;
	u8 *dest = decoded2;

	for (int patch_idx = 0; patch_idx < num_patches_u*num_patches_v; ++patch_idx) {
		HWSplinePatch& patch = patches[patch_idx];

		// TODO: Should do actual patch subdivision instead of just drawing the control points!
		const int tile_min_u = (patch.type & START_OPEN_U) ? 0 : 1;
		const int tile_min_v = (patch.type & START_OPEN_V) ? 0 : 1;
		const int tile_max_u = (patch.type & END_OPEN_U) ? 3 : 2;
		const int tile_max_v = (patch.type & END_OPEN_V) ? 3 : 2;
		for (int tile_u = tile_min_u; tile_u < tile_max_u; ++tile_u) {
			for (int tile_v = tile_min_v; tile_v < tile_max_v; ++tile_v) {
				int point_index = tile_u + tile_v*4;

				u8 *v0 = patch.points[point_index];
				u8 *v1 = patch.points[point_index+1];
				u8 *v2 = patch.points[point_index+4];
				u8 *v3 = patch.points[point_index+5];

				// TODO: Insert UVs where applicable. Actually subdivide.
				CopyTriangle(dest, v0, v1, v2, undecodedVertexSize);
				CopyTriangle(dest, v2, v1, v0, undecodedVertexSize);
				CopyTriangle(dest, v2, v1, v3, undecodedVertexSize);
				CopyTriangle(dest, v3, v1, v2, undecodedVertexSize);
				count += 12;
			}
		}
	}
	delete[] patches;

	u32 vertTypeWithoutIndex = vertex_type & ~GE_VTYPE_IDX_MASK;

	SubmitPrim(decoded2, 0, GE_PRIM_TRIANGLES, count, vertTypeWithoutIndex, GE_VTYPE_IDX_NONE, 0);
	Flush();
}