/*
* Descent 3: Piccu Engine
* Copyright (C) 2024 SaladBadger
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <algorithm>
#include "descent.h"
#include "newrender.h"
#include "render.h"
#include "3d.h"
#include "renderer.h"
#include "room.h"
#include "../renderer/gl_mesh.h"
#include "pserror.h"
#include "special_face.h"
#include "terrain.h"
#include "lightmap_info.h"

//[ISB] Checks if a face is completely static and therefore should be in the normal static meshes.
//Portals need to be put into another pass because they may or may not be visible. 
static inline bool FaceIsStatic(room& rp, face& fp)
{
	//Check for a floating trigger, which doesn't get rendered
	if ((fp.flags & FF_FLOATING_TRIG))
		return false;

	//No portals in the normal interactions. 
	//Portal faces will be put in another list since they need to be determined at runtime. 
	if (fp.portal_num != -1)
		return false;

	//Nothing special, so face renders
	return true;
}

//Determines if a face draws with alpha blending
//Parameters:	fp - pointer to the face in question
//					bm_handle - the handle for the bitmap for this frame, or -1 if don't care about transparence
//Returns:		bitmask describing the alpha blending for the face
//					the return bits are the ATF_ flags in renderer.h
static inline int GetFaceAlpha(face* fp, int bm_handle)
{
	int ret = AT_ALWAYS;
	if (GameTextures[fp->tmap].flags & TF_SATURATE)
	{
		ret = AT_SATURATE_TEXTURE;
	}
	else
	{
		//Check the face's texture for an alpha value
		if (GameTextures[fp->tmap].alpha < 1.0)
			ret |= ATF_CONSTANT;

		//Check for transparency
		if (bm_handle >= 0 && GameBitmaps[bm_handle].format != BITMAP_FORMAT_4444 && GameTextures[fp->tmap].flags & TF_TMAP2)
			ret |= ATF_TEXTURE;
	}
	return ret;
}

//Changes that can happen to a face to warrant a remesh
struct FacePrevState
{
	int flags;
	int tmap;
};

//Future profiling: Given the dynamic nature of rooms, does it make sense to have only one large vertex buffer?
//Or would eating the cost of rebinds be paid for by more efficient generation of room meshes?
VertexBuffer Room_VertexBuffer;
IndexBuffer Room_IndexBuffer;

struct RoomDrawElement
{
	int texturenum;
	int lmhandle;
	ElementRange range;
};

struct SpecularDrawElement
{
	int texturenum;
	int lmhandle;
	ElementRange range;
	special_face* special;
};

struct RoomMesh
{
	int roomnum;
	std::vector<RoomDrawElement> LitInteractions;
	std::vector<RoomDrawElement> UnlitInteractions;
	std::vector<SpecularDrawElement> SpecInteractions;
	std::vector<RoomDrawElement> MirrorInteractions;
	//One of these for each face.
	//If the state of FacePrevStates[facenum] != roomptr->faces[facenum], remesh this part of the world. Sigh.
	std::vector<FacePrevState> FacePrevStates;

	uint32_t FirstVertexOffset;
	uint32_t FirstVertex;
	uint32_t FirstIndexOffset;
	uint32_t FirstIndex;
	void ResetInteractions()
	{
		LitInteractions.clear();
		UnlitInteractions.clear();
		SpecInteractions.clear();
		MirrorInteractions.clear();
	}

	void Reset()
	{
		ResetInteractions();
		FacePrevStates.clear();
	}

	void DrawLit()
	{
		for (RoomDrawElement& element : LitInteractions)
		{
			//Bind bitmaps. Temp API, should the bitmap system also handle binding? Or does that go elsewhere?
			Room_VertexBuffer.BindBitmap(GetTextureBitmap(element.texturenum, 0));
			Room_VertexBuffer.BindLightmap(element.lmhandle);

			//And draw
			Room_VertexBuffer.DrawIndexed(element.range);
		}
	}

	void DrawUnlit()
	{
		for (RoomDrawElement& element : UnlitInteractions)
		{
			//Bind bitmaps. Temp API, should the bitmap system also handle binding? Or does that go elsewhere?
			Room_VertexBuffer.BindBitmap(GetTextureBitmap(element.texturenum, 0));

			//And draw
			Room_VertexBuffer.DrawIndexed(element.range);
		}
	}

	void DrawMirrorFaces()
	{
		if (MirrorInteractions.size() == 0)
			return;

		assert(Rooms[roomnum].mirror_face != -1);
		Room_VertexBuffer.BindBitmap(GetTextureBitmap(Rooms[roomnum].faces[Rooms[roomnum].mirror_face].tmap, 0));
		for (RoomDrawElement& element : MirrorInteractions)
		{
			Room_VertexBuffer.BindLightmap(element.lmhandle);

			//And draw
			Room_VertexBuffer.DrawIndexed(element.range);
		}
	}

	void DrawSpecular()
	{
		int last_texture = -1;
		int last_lightmap = -1;
		static SpecularBlock specblock;
		if (Rooms[roomnum].flags & RF_EXTERNAL)
		{
			for (SpecularDrawElement& element : SpecInteractions)
			{
				//External rooms only can have speculars to one satellite in the sky, and it's always white. 
				//But it emits colored light? heh. 
				specblock.num_speculars = 1;
				specblock.speculars[0].bright_center[0] = Terrain_sky.satellite_vectors[0].x;
				specblock.speculars[0].bright_center[1] = Terrain_sky.satellite_vectors[0].y;
				specblock.speculars[0].bright_center[2] = Terrain_sky.satellite_vectors[0].z;
				specblock.speculars[0].bright_center[3] = 1;
				specblock.speculars[0].color[2] =
					specblock.speculars[0].color[1] =
					specblock.speculars[0].color[0] = 1.0f;

				//Bind bitmaps. Temp API, should the bitmap system also handle binding? Or does that go elsewhere?
				if (element.texturenum != last_texture)
				{
					last_texture = element.texturenum;
					Room_VertexBuffer.BindBitmap(GetTextureBitmap(element.texturenum, 0));
					if (GameTextures[element.texturenum].flags & TF_SMOOTH_SPECULAR)
						specblock.strength = 1;
					else
						specblock.strength = 4;

					if (GameTextures[element.texturenum].flags & TF_PLASTIC)
						specblock.exponent = 14;
					else if (GameTextures[element.texturenum].flags & TF_MARBLE)
						specblock.exponent = 4;
					else
						specblock.exponent = 6;
				}

				if (element.lmhandle != last_lightmap)
				{
					last_lightmap = element.lmhandle;
					Room_VertexBuffer.BindLightmap(element.lmhandle);
				}

				rend_UpdateSpecular(&specblock);

				//And draw
				Room_VertexBuffer.DrawIndexed(element.range);
			}
		}
		else
		{
			for (SpecularDrawElement& element : SpecInteractions)
			{
				//Bind bitmaps. Temp API, should the bitmap system also handle binding? Or does that go elsewhere?
				if (element.texturenum != last_texture)
				{
					last_texture = element.texturenum;
					Room_VertexBuffer.BindBitmap(GetTextureBitmap(element.texturenum, 0));
					if (GameTextures[element.texturenum].flags & TF_SMOOTH_SPECULAR)
						specblock.strength = 1;
					else
						specblock.strength = 4;

					if (GameTextures[element.texturenum].flags & TF_PLASTIC)
						specblock.exponent = 14;
					else if (GameTextures[element.texturenum].flags & TF_MARBLE)
						specblock.exponent = 4;
					else
						specblock.exponent = 6;
				}

				if (element.lmhandle != last_lightmap)
				{
					last_lightmap = element.lmhandle;
					Room_VertexBuffer.BindLightmap(element.lmhandle);
				}

				specblock.num_speculars = element.special->num;
				for (int i = 0; i < specblock.num_speculars; i++) //aaaaaaa
				{
					specblock.speculars[i].bright_center[0] = element.special->spec_instance[i].bright_center.x;
					specblock.speculars[i].bright_center[1] = element.special->spec_instance[i].bright_center.y;
					specblock.speculars[i].bright_center[2] = element.special->spec_instance[i].bright_center.z;
					specblock.speculars[i].bright_center[3] = 1;
					specblock.speculars[i].color[2] = (element.special->spec_instance[i].bright_color & 31) / 31.f;
					specblock.speculars[i].color[1] = ((element.special->spec_instance[i].bright_color >> 5) & 31) / 31.f;
					specblock.speculars[i].color[0] = ((element.special->spec_instance[i].bright_color >> 10) & 31) / 31.f;
				}

				rend_UpdateSpecular(&specblock);

				//And draw
				Room_VertexBuffer.DrawIndexed(element.range);
			}
		}
	}
};


//[ISB] temp: Should stick this in Room or a separate render-related struct later down the line (dynamic room limit at some point?)
//These are the meshes of all normal room geometry. 
RoomMesh Room_meshes[MAX_ROOMS];

void AddFacesToBuffer(MeshBuilder& mesh, std::vector<SortableElement>& elements, std::vector<RoomDrawElement>& interactions, room& rp, int indexOffset, int firstIndex)
{
	if (elements.empty())
		return;

	int lasttmap = -1;
	int lastlm = -1;
	bool firsttime = true;
	int triindices[3];
	RendVertex vert;
	float alpha = 1;
	for (SortableElement& element : elements)
	{
		if (element.texturehandle != lasttmap || element.lmhandle != lastlm)
		{
			if (!firsttime)
			{
				mesh.EndVertices();
				RoomDrawElement element;
				element.texturenum = lasttmap;
				element.lmhandle = lastlm;
				element.range = mesh.EndIndices();
				element.range.offset += firstIndex;
				interactions.push_back(element);
			}
			else
				firsttime = false;

			mesh.BeginVertices();
			mesh.BeginIndices();
			lasttmap = element.texturehandle;
			lastlm = element.lmhandle;

			vert.uslide = GameTextures[lasttmap].slide_u;
			vert.vslide = GameTextures[lasttmap].slide_v;

			alpha = GameTextures[element.texturehandle].alpha;
		}

		face& fp = rp.faces[element.element];

		int first_index = mesh.NumVertices() + indexOffset;
		for (int i = 0; i < fp.num_verts; i++)
		{
			roomUVL uvs = fp.face_uvls[i];
			vert.position = rp.verts[fp.face_verts[i]];
			vert.normal = fp.normal; //oh no, no support for phong shading..
			vert.r = vert.g = vert.b = 255;
			vert.a = (ubyte)(std::min(1.f, std::max(0.f, alpha)) * 255);
			vert.u1 = uvs.u; vert.v1 = uvs.v;
			vert.u2 = uvs.u2; vert.v2 = uvs.v2;

			mesh.AddVertex(vert);
		}

		//Generate indicies as a triangle fan
		for (int i = 2; i < fp.num_verts; i++)
		{
			triindices[0] = first_index;
			triindices[1] = first_index + i - 1;
			triindices[2] = first_index + i;
			mesh.SetIndicies(3, triindices);
		}
	}

	mesh.EndVertices();
	RoomDrawElement element;
	element.texturenum = lasttmap;
	element.lmhandle = lastlm;
	element.range = mesh.EndIndices();
	element.range.offset += firstIndex;
	interactions.push_back(element);
}

void AddSpecFacesToBuffer(MeshBuilder& mesh, std::vector<SortableElement>& elements, std::vector<SpecularDrawElement>& interactions, room& rp, int indexOffset, int firstIndex)
{
	if (elements.empty())
		return;

	int lasttmap = -1;
	int lastlm = -1;
	bool firsttime = true;
	int triindices[3];
	RendVertex vert;
	for (SortableElement& element : elements)
	{
		mesh.BeginVertices();
		mesh.BeginIndices();
		lasttmap = element.texturehandle;
		lastlm = element.lmhandle;

		vert.uslide = GameTextures[lasttmap].slide_u;
		vert.vslide = GameTextures[lasttmap].slide_v;

		face& fp = rp.faces[element.element];

		int first_index = mesh.NumVertices() + indexOffset;
		if (GameTextures[element.texturehandle].flags & TF_SMOOTH_SPECULAR && fp.special_handle != BAD_SPECIAL_FACE_INDEX)
		{
			for (int i = 0; i < fp.num_verts; i++)
			{
				roomUVL uvs = fp.face_uvls[i];
				vert.position = rp.verts[fp.face_verts[i]];
				vert.normal = SpecialFaces[fp.special_handle].vertnorms[i];
				vert.r = vert.g = vert.b = vert.a = 255;
				vert.u1 = uvs.u; vert.v1 = uvs.v;
				vert.u2 = uvs.u2; vert.v2 = uvs.v2;

				mesh.AddVertex(vert);
			}
		}
		else
		{
			for (int i = 0; i < fp.num_verts; i++)
			{
				roomUVL uvs = fp.face_uvls[i];
				vert.position = rp.verts[fp.face_verts[i]];
				vert.normal = fp.normal;
				vert.r = vert.g = vert.b = vert.a = 255;
				vert.u1 = uvs.u; vert.v1 = uvs.v;
				vert.u2 = uvs.u2; vert.v2 = uvs.v2;

				mesh.AddVertex(vert);
			}
		}

		//Generate indicies as a triangle fan
		for (int i = 2; i < fp.num_verts; i++)
		{
			triindices[0] = first_index;
			triindices[1] = first_index + i - 1;
			triindices[2] = first_index + i;
			mesh.SetIndicies(3, triindices);
		}

		mesh.EndVertices();
		SpecularDrawElement element;
		element.texturenum = lasttmap;
		element.lmhandle = lastlm;
		element.range = mesh.EndIndices();
		element.range.offset += firstIndex;
		element.special = &SpecialFaces[fp.special_handle];
		interactions.push_back(element);
	}
}

//Meshes a given room. 
//Index offset is added to all generated indicies, to allow updating a room at a specific place
//later down the line, even with an empty MeshBuilder. 
//First index is added to all interactions to indicate where the first index to draw is. 
void UpdateRoomMesh(MeshBuilder& mesh, int roomnum, int indexOffset, int firstIndex)
{
	room& rp = Rooms[roomnum];
	if (!rp.used)
		return; //unused room

	//Maybe these should be changed into one pass using a white texture for unlit?
	//But what happens if something silly like HDR lighting is added later?
	std::vector<SortableElement> faces_lit;
	std::vector<SortableElement> faces_unlit;
	std::vector<SortableElement> faces_spec;
	std::vector<SortableElement> faces_mirror;
	std::vector<int> faces_special;

	RoomMesh& roommesh = Room_meshes[roomnum];
	if (roommesh.FacePrevStates.size() != rp.num_faces)
		roommesh.FacePrevStates.resize(rp.num_faces);

	roommesh.roomnum = roomnum;

	roommesh.ResetInteractions();

	//Mirrors are defined as "the mirror face and every other face that happens to share the same texture"
	uint32_t mirror_tex_hack = UINT32_MAX;
	if (rp.mirror_face != -1)
	{
		mirror_tex_hack = rp.faces[rp.mirror_face].tmap;
	}

	//Build a sortable list of all faces
	for (int i = 0; i < rp.num_faces; i++)
	{
		face& fp = rp.faces[i];
		roommesh.FacePrevStates[i].flags = fp.flags;
		roommesh.FacePrevStates[i].tmap = fp.tmap;
		if (!FaceIsStatic(rp, fp))
		{
			faces_special.push_back(i);
			continue;
		}

		//blarg
		int bm_handle = GetTextureBitmap(fp.tmap, 0);
		int alphatype = GetFaceAlpha(&fp, -1);
		/*if ((alphatype & (ATF_CONSTANT | ATF_TEXTURE)) != 0)
			continue;
		else*/
		{
			int tmap = fp.tmap;
			if (fp.flags & FF_DESTROYED && GameTextures[tmap].flags & TF_DESTROYABLE)
				tmap = GameTextures[tmap].destroy_handle;

			//Not a postrender, determine if it is unlit or lit. 
			if (rp.mirror_face != -1 && tmap == mirror_tex_hack)
			{
				faces_mirror.push_back(SortableElement{ i, (ushort)tmap, LightmapInfo[fp.lmi_handle].lm_handle });
			}
			else if (fp.flags & FF_LIGHTMAP)
			{
				//If the face is specular, add it for a post stage. 
				//Specs have to be in a special pass like this so that the size of the room vertex buffer never changes
				//External specular faces don't use a special face, and therefore can never be smooth. Heh. 
				if (GameTextures[tmap].flags & TF_SPECULAR && (fp.special_handle != BAD_SPECIAL_FACE_INDEX || (rp.flags & RF_EXTERNAL)))
				{
					faces_spec.push_back(SortableElement{ i, (ushort)tmap, LightmapInfo[fp.lmi_handle].lm_handle });
				}
				else
				{
					//TODO: Add field names when Piccu becomes C++20.
					faces_lit.push_back(SortableElement{ i, (ushort)tmap, LightmapInfo[fp.lmi_handle].lm_handle });
				}
			}
			else
			{
				faces_unlit.push_back(SortableElement{ i, (ushort)tmap, 0 });
			}
		}
	}

	std::sort(faces_lit.begin(), faces_lit.end());
	AddFacesToBuffer(mesh, faces_lit, Room_meshes[roomnum].LitInteractions, rp, indexOffset, firstIndex);

	std::sort(faces_unlit.begin(), faces_unlit.end());
	AddFacesToBuffer(mesh, faces_unlit, Room_meshes[roomnum].UnlitInteractions, rp, indexOffset, firstIndex);

	std::sort(faces_mirror.begin(), faces_mirror.end());
	AddFacesToBuffer(mesh, faces_mirror, Room_meshes[roomnum].MirrorInteractions, rp, indexOffset, firstIndex);

	//Even though they're not batched up (may be fixable if I can quickly determine if they have identical light sources), 
	//sort specular faces to try to minimize texture state thrashing. Even though that's trivial compared to the buffer state thrashing. 
	std::sort(faces_spec.begin(), faces_spec.end());
	AddSpecFacesToBuffer(mesh, faces_spec, Room_meshes[roomnum].SpecInteractions, rp, indexOffset, firstIndex);
}

void FreeRoomMeshes()
{
	for (int i = 0; i < MAX_ROOMS; i++)
	{
		Room_meshes[i].Reset();
	}
	Room_VertexBuffer.Destroy();
	Room_IndexBuffer.Destroy();
}

uint32_t lightmap_room_handle = 0xFFFFFFFFu;
uint32_t lightmap_specular_handle = 0xFFFFFFFFu;
uint32_t lightmap_room_fog_handle = 0xFFFFFFFFu;
uint32_t lightmap_room_specular_fog_handle = 0xFFFFFFFFu;

//Called during LoadLevel, builds meshes for every room. 
void MeshRooms()
{
	if (lightmap_specular_handle == 0xFFFFFFFFu)
	{
		lightmap_specular_handle = rend_GetPipelineByName("lightmapped_specular");
		assert(lightmap_specular_handle != 0xFFFFFFFFu);
	}
	if (lightmap_room_fog_handle == 0xFFFFFFFFu)
	{
		lightmap_room_fog_handle = rend_GetPipelineByName("lightmap_room_fog");
		assert(lightmap_room_fog_handle != 0xFFFFFFFFu);
	}
	if (lightmap_room_handle == 0xFFFFFFFFu)
	{
		lightmap_room_handle = rend_GetPipelineByName("lightmap_room");
		assert(lightmap_room_handle != 0xFFFFFFFFu);
	}
	if (lightmap_room_specular_fog_handle == 0xFFFFFFFFu)
	{
		lightmap_room_specular_fog_handle = rend_GetPipelineByName("lightmap_room_specular_fog");
		assert(lightmap_room_specular_fog_handle != 0xFFFFFFFFu);
	}
	MeshBuilder mesh;
	FreeRoomMeshes();
	for (int i = 0; i <= Highest_room_index; i++)
	{
		//These can be set here and should remain static, since the amount of vertices and indices should remain static across any room changes
		Room_meshes[i].FirstVertexOffset = mesh.VertexOffset();
		Room_meshes[i].FirstVertex = mesh.NumVertices();
		Room_meshes[i].FirstIndex = mesh.NumIndices();
		Room_meshes[i].FirstIndexOffset = mesh.IndexOffset();

		UpdateRoomMesh(mesh, i, 0, 0);
	}

	mesh.BuildVertices(Room_VertexBuffer);
	mesh.BuildIndicies(Room_IndexBuffer);
}

//Returns true if the room at roomnum needs to have its static mesh regenerated. 
//Because nothing can truly be static when the original rendering code was fully dynamic.
static bool RoomNeedRemesh(int roomnum)
{
	room& rp = Rooms[roomnum];
	RoomMesh& mesh = Room_meshes[roomnum];
	for (int i = 0; i < rp.num_faces; i++)
	{
		if ((rp.faces[i].flags & FF_DESTROYED) != (mesh.FacePrevStates[i].flags & FF_DESTROYED))
			return true;

		if (rp.faces[i].tmap != mesh.FacePrevStates[i].tmap)
			return true;
	}
	return false;
}

static void RemeshRoom(MeshBuilder& mesh, int roomnum)
{
	mprintf((0, "RemeshRoom: Updating room %d\n", roomnum));
	mesh.Destroy();
	UpdateRoomMesh(mesh, roomnum, Room_meshes[roomnum].FirstVertex, Room_meshes[roomnum].FirstIndex);

	mesh.UpdateVertices(Room_VertexBuffer, Room_meshes[roomnum].FirstVertexOffset);
	mesh.UpdateIndicies(Room_IndexBuffer, Room_meshes[roomnum].FirstIndexOffset);
}

struct NewRenderPassInfo
{
	//Pointer to the shader handle that will be used for this pass.
	uint32_t& handle;
	//True if only fog rooms should be rendered.
	bool fog;
	//True if only specular faces should be rendered.
	bool specular;
} renderpass_info[] =
{
	{lightmap_room_handle, false, false},
	{lightmap_room_handle, false, false},
	{lightmap_specular_handle, false, true},
	{lightmap_room_fog_handle, true, false},
	{lightmap_room_fog_handle, true, false},
	{lightmap_room_specular_fog_handle, true, true},
};

//I'm begging you please switch to a newer spec so you can use std::array with deduction guides. 
//Actually would that work with a composite type here? Actually would this just be another use for a C#-like array class?
#define NUM_NEWRENDERPASSES sizeof(renderpass_info) / sizeof(renderpass_info[0])

void ComputeRoomPulseLight(room* rp);

//Performs tasks that need to be done before rendering a room.
void NewRenderPreDraw()
{
	/*RoomBlock roomblocks[MAX_RENDER_ROOMS];
	for (int nn = N_render_rooms - 1; nn >= 0; nn--)
	{
		int roomnum = Render_list[nn];
		room& rp = Rooms[roomnum];
		RoomBlock& roomblock = roomblocks[nn];

		// Mark it visible for automap
		AutomapVisMap[&rp - Rooms] = 1;

		ComputeRoomPulseLight(&Rooms[roomnum]);
		roomblock.brightness = Room_light_val;

		if (rp.flags & RF_FOG)
		{
			SetupRoomFog(&rp, &Viewer_eye, &Viewer_orient, Viewer_roomnum);

			roomblock.fog_distance = rp.fog_depth;
			roomblock.fog_color[0] = rp.fog_r;
			roomblock.fog_color[1] = rp.fog_g;
			roomblock.fog_color[2] = rp.fog_b;

			if (Room_fog_plane_check == 0)
			{
				roomblock.not_in_room = true;
				roomblock.fog_plane[0] = Room_fog_plane.x;
				roomblock.fog_plane[1] = Room_fog_plane.y;
				roomblock.fog_plane[2] = Room_fog_plane.z;
				roomblock.fog_plane[3] = Room_fog_distance;
			}
			else
			{
				roomblock.not_in_room = false;
			}
		}

		rp.last_render_time = Gametime;
		rp.flags &= ~RF_MIRROR_VISIBLE;

		for (int facenum = 0; facenum < rp.num_faces; facenum++)
		{
			face& fp = rp.faces[facenum];
			if (!(fp.flags & FF_NOT_FACING))
			{
				fp.renderframe = FrameCount & 0xFF;
			}
		}
	}

	rend_UpdateFogBrightness(roomblocks, N_render_rooms);*/
}

void DoNewRenderPass(int passnum)
{
	/*assert(passnum >= 0 && passnum < NUM_NEWRENDERPASSES);
	NewRenderPassInfo& passinfo = renderpass_info[passnum];
	static RoomBlock roomblock;

	rend_BindPipeline(passinfo.handle);

	for (int nn = N_render_rooms - 1; nn >= 0; nn--)
	{
		int roomnum = Render_list[nn];
		room& rp = Rooms[roomnum];
		ComputeRoomPulseLight(&Rooms[roomnum]);

		if (passinfo.fog)
		{
			if (Detail_settings.Fog_enabled && !(Rooms[roomnum].flags & RF_FOG))
				continue;
		}
		else
		{
			if (Detail_settings.Fog_enabled && Rooms[roomnum].flags & RF_FOG)
				continue;
		}

		rend_SetCurrentRoomNum(nn);

		if (passinfo.specular)
			Room_meshes[roomnum].DrawSpecular();
		else
		{
			Room_meshes[roomnum].DrawLit();
			//Room_meshes[roomnum].DrawMirrorFaces();
		}

		//TEMP mirror test
		if (!passinfo.specular)
		{
			if (rp.mirror_face != -1)
			{
				g3Plane plane(rp.faces[rp.mirror_face].normal, rp.verts[rp.faces[rp.mirror_face].face_verts[0]]);
				float reflectmat[16];
				g3_GenerateReflect(plane, reflectmat);
				g3_StartInstanceMatrix4(reflectmat);

				Room_meshes[roomnum].DrawLit();

				g3_DoneInstance();
			}
		}
	}*/
}

void NewRender_Render(vector& vieweye, matrix& vieworientation)
{
	/*//Set up rendering states
	//I hate this global state thing how can I make it better
	rend_SetColorModel(CM_MONO);
	rend_SetLighting(LS_GOURAUD);
	rend_SetWrapType(WT_WRAP);


	Room_VertexBuffer.Bind();
	Room_IndexBuffer.Bind();

	//Walk the room render list for updates
	for (int nn = N_render_rooms - 1; nn >= 0; nn--)
	{
		MeshBuilder mesh;
		int roomnum = Render_list[nn];
		if (RoomNeedRemesh(roomnum))
		{
			RemeshRoom(mesh, roomnum);
		}
	}

	rend_SetAlphaType(AT_ALWAYS);

	NewRenderPreDraw();
	//TODO: fix magic numbers
	DoNewRenderPass(0);
	DoNewRenderPass(2);
	DoNewRenderPass(3);
	DoNewRenderPass(5);

	rendTEMP_UnbindVertexBuffer();
	rend_EndShaderTest();*/
}

void NewRender_InitNewLevel()
{
}

void RenderList::AddRoom(int roomnum, Frustum& frustum)
{
	//Mark it as visible
	RoomChecked[roomnum] = true;
	VisibleRoomNums.push_back(roomnum);
}

RenderList::RenderList()
{
	CurrentCheck = 0;
}

void RenderList::GatherVisible(vector& eye_pos, int viewroomnum)
{
	//Initialize the room checked list
	RoomChecked.clear();
	RoomChecked.resize(Highest_room_index);
	VisibleRoomNums.clear();

	CurrentCheck = 0;

	Frustum viewFrustum(gTransformFull);

	if (viewroomnum >= 0) //is a room?
	{
		AddRoom(viewroomnum, viewFrustum);
	}

	while (PendingRooms())
	{
		int roomnum = PopRoom();
		AddRoom(roomnum, viewFrustum);
	}
}
