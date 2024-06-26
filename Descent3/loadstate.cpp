/* 
* Descent 3 
* Copyright (C) 2024 Parallax Software
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

#include "gamesave.h"
#include "descent.h"
#include "CFILE.H"
#include "Mission.h"
#include "gamesequence.h"
#include "gameevent.h"
#include "gameloop.h"
#include "game.h"
#include "object.h"
#include "objinfo.h"
#include "gametexture.h"
#include "bitmap.h"
#include "ship.h"
#include "door.h"
#include "stringtable.h"
#include "weapon.h"
#include "vclip.h"
#include "viseffect.h"
#include "room.h"
#include "trigger.h"
#include "spew.h"
#include "doorway.h"
#include "AIMain.h"
#include "mem.h"
#include "osiris_dll.h"
#include "terrain.h"
#include <string.h>
#include "levelgoal.h"
#include "aistruct.h"
#include "matcen.h"
#include "pilot.h"
#include "marker.h"
#include "d3music.h"
#include "weather.h"
#include "cockpit.h"
#include "hud.h"

void PageInAllData ();

// dynamically allocated to be efficient (only needed during save/load)

int LGSSnapshot(CFILE *fp);



int Times_game_restored = 0;
//static gs_tables *gs_Xlates = NULL;

gs_tables *gs_Xlates = NULL;
//int Gamesave_read_version=0;


void IncreaseRestoreCount(const char *file)
{
	//Open the file up in read more, read the current count, then incease it 
	//and write the increased value back out.

	char countpath[_MAX_PATH*2];
	CFILE *cfp;

	strcpy(countpath,file);
	strcat(countpath,".cnt");

	cfp = cfopen(countpath,"rb");
	if(cfp)
	{
		Times_game_restored = cf_ReadInt(cfp);
		cfclose(cfp);
	}
	else
	{
		Times_game_restored = 0;
	}
	Times_game_restored++;

	cfp = cfopen(countpath,"wb");
	if(cfp)
	{
		cf_WriteInt(cfp,Times_game_restored);
		cfclose(cfp);
	}
}



extern bool IsRestoredGame;
///////////////////////////////////////////////////////////////////////////////
//	loads a game from a given slot.
int LoadGameState(const char *pathname)
{
	CFILE *fp;
	int retval = LGS_OK;
	char desc[GAMESAVE_DESCLEN+1];
	char path[PSPATHNAME_LEN];
	ushort version;
	ushort curlevel;
	short pending_music_region;
	IsRestoredGame = true;
//	load in stuff
	fp = cfopen(pathname, "rb");
	if (!fp)
	{
		Int3();
		return LGS_FILENOTFOUND;
	}

	
START_VERIFY_SAVEFILE(fp);
	gs_Xlates = new gs_tables;

// read in header and do version check.
	cf_ReadBytes((ubyte *)desc, sizeof(desc), fp);
	version = (ushort)cf_ReadShort(fp);
	if (version < GAMESAVE_OLDVER) 
	{
		Int3();
		retval = LGS_OUTDATEDVER;
		goto loadsg_error;
	}

	retval = LGSSnapshot(fp);						// read and clear snapshot.
	if (retval > 0) 
	{
		bm_FreeBitmap(retval);
	}

	//Gamesave_read_version=version;

// read translation tables
	retval = LGSXlateTables(fp);
	if (retval != LGS_OK)
		goto loadsg_error;
//	read in gamemode info.

//	read in mission level info.
	curlevel = (ushort)cf_ReadShort(fp);
	cf_ReadString(path, sizeof(path), fp);

	if( (curlevel > 4) && (strcmpi(path,"d3.mn3")==0) )
		strcpy(path,"d3_2.mn3");

//	must load mission and initialize level before reading in other data.
	retval = LGSMission(path, curlevel);
	if (retval != LGS_OK) goto loadsg_error;

	Current_mission.game_state_flags = cf_ReadInt(fp);
	//Increase count for how many times this file was restored
	IncreaseRestoreCount(pathname);

// read in information after level's been loaded.
	Gametime = cf_ReadFloat(fp);
	FrameCount = cf_ReadInt(fp);
	Current_waypoint = cf_ReadInt(fp);
	pending_music_region = cf_ReadShort(fp);
	D3MusicSetRegion(pending_music_region);

	//Times_game_restored = cf_ReadInt(fp);
	//Read Weather
	{
		int weather_size = cf_ReadInt(fp);
		if(weather_size != sizeof(Weather))
		{
			Int3();
			retval = LGS_OUTDATEDVER;
			goto loadsg_error;
		}
	}
	cf_ReadBytes((ubyte *)&Weather,sizeof(Weather),fp);

	//Restore active doorways
	{
		int num_active_dw = cf_ReadInt(fp);
		Num_active_doorways = cf_ReadInt(fp);
		
		for(int d=0;d<num_active_dw;d++)
		{
			Active_doorways[d] = cf_ReadInt(fp);
		}

	}

// read in room info
	retval = LGSRooms(fp);
	if (retval != LGS_OK) goto loadsg_error;

// read in trigger info
	retval = LGSTriggers(fp);
	if (retval != LGS_OK) goto loadsg_error;

// read in object info.
	retval = LGSObjects(fp, version);
	if (retval != LGS_OK) goto loadsg_error;	

// read in  players info
	retval = LGSPlayers(fp);
	if (retval != LGS_OK) goto loadsg_error;

// read in matcen info.
	retval = LGSMatcens(fp);
	if (retval != LGS_OK) goto loadsg_error;

// read in viseffecsts
	retval = LGSVisEffects(fp);
	if (retval != LGS_OK) goto loadsg_error;

// read in spew
	retval = LGSSpew(fp);
	if (retval != LGS_OK) goto loadsg_error;


//Load OSIRIS Stuff
	if(!Osiris_RestoreSystemState(fp))
	{
		goto loadsg_error;
	}

	//Load Level goal info
	Level_goals.LoadLevelGoalInfo(fp);

// read in game messages for console. (must occur AFTER players are read in!!!)
	LGSGameMessages(fp);

// loads and restores hud state.
	if (!LGSHudState(fp)) 
	{
		retval = LGS_OBJECTSCORRUPT;
		goto loadsg_error;
	}

loadsg_error:
	delete gs_Xlates;
	gs_Xlates = nullptr;

	END_VERIFY_SAVEFILE(fp, "Total load");	
	cfclose(fp);

	
	//Page everything in here!
	PageInAllData ();

	IncrementPilotRestoredGamesForMission(&Current_pilot,Current_mission.name);

	return retval;
}


//	retreive gamesave file header info. description must be a buffer of length GAMESAVE_DESCLEN+1
// returns true if it's a valid savegame file.  false if corrupted somehow
bool GetGameStateInfo(const char *pathname, char *description, int *bm_handle)
{
	CFILE *fp;
	int bitmap;
	char desc[GAMESAVE_DESCLEN+1];

	fp = cfopen(pathname, "rb");
	if (!fp)
		return false;

	if (!cf_ReadBytes((ubyte *)desc, GAMESAVE_DESCLEN+1, fp)) 
	{
		strcpy(description, TXT_ILLEGALSAVEGAME);
		goto savesg_error;
	}

	strcpy(description, desc);

	if (bm_handle) 
	{
		cf_ReadShort(fp);					// skip version.
		bitmap = LGSSnapshot(fp);
		*bm_handle = bitmap;
	}

	cfclose(fp);
	return true;

savesg_error:
	cfclose(fp);
	return false;
}



//////////////////////////////////////////////////////////////////////////////
#define BUILD_XLATE_TABLE(_table, _maxtable, _fn)	do { gs_ReadShort(fp, num); for (i = 0; i < num; i++) {	cf_ReadString(name, sizeof(name), fp); index = _fn(name); _table[i] = (index == -1) ? 0 : index;} for (;i < _maxtable; i++) _table[i] = 0; } while (0)

#define BUILD_MINI_XLATE_TABLE(_table, _fn) memset(_table, 0, sizeof(_table)); \
	do { \
		gs_ReadShort(fp, i); \
		cf_ReadString(name, sizeof(name), fp); \
		if (i == -1 && name[0] == 0) break; \
		index = _fn(name); \
		_table[i] = (index==-1) ? 0 : index; \
	} while(1)


//	reads in translation tables
int LGSXlateTables(CFILE *fp)
{
START_VERIFY_SAVEFILE(fp);
	int retval = LGS_OK;
	short i, num,index;
	char name[64];

//	load object info translation table
	BUILD_XLATE_TABLE(gs_Xlates->obji_indices, MAX_OBJECT_IDS, FindObjectIDName);

//	load polymodel translation list.
	BUILD_XLATE_TABLE(gs_Xlates->model_handles, MAX_POLY_MODELS, FindPolyModelName);

// load doar translation list.
	BUILD_XLATE_TABLE(gs_Xlates->door_handles, MAX_DOORS, FindDoorName);

// load ship translation list.
	BUILD_XLATE_TABLE(gs_Xlates->ship_handles, MAX_SHIPS, FindShipName);

// build weapon translation list
	BUILD_XLATE_TABLE(gs_Xlates->wpn_handles, MAX_WEAPONS, FindWeaponName);

// read in limited texture name list.  
	BUILD_XLATE_TABLE(gs_Xlates->tex_handles, MAX_TEXTURES, FindTextureName);

// read in limited bitmap name list.  this is a slightly diff format than above to save space
	BUILD_MINI_XLATE_TABLE(gs_Xlates->bm_handles, bm_FindBitmapName);

END_VERIFY_SAVEFILE(fp, "Xlate load");
	return retval;
}


//	loads in level's mission and level.
int LGSMission(const char *msnname, int level)
{
//	we will free the mission.
//	Free any game objects/etc that needs to be done when ending a level here.
	FreeScriptsForLevel();

	Osiris_DisableCreateEvents();
	if (LoadMission((const char *)msnname)) 
	{
		SetCurrentLevel(level);
		Player_num=0;	// Reset player num
		Players[Player_num].ship_index = FindShipName(DEFAULT_SHIP);
		ASSERT(Players[Player_num].ship_index != -1);

		// Load any addon data
		mng_LoadAddonPages();

		InitPlayerNewShip(Player_num,INVRESET_ALL);
		InitCameraViews(1);	 //Turn off all camera views, including rear views

		if (!LoadAndStartCurrentLevel()) 
		{
			Int3();
			Osiris_EnableCreateEvents();
			return LGS_STARTLVLFAILED;
		}
	}
	else 
	{
		Int3();
		Osiris_EnableCreateEvents();
		return LGS_MISSIONFAILED;
	}
	Osiris_EnableCreateEvents();
	//mng_LoadAddonPages ();
	return LGS_OK;
}

extern ubyte AutomapVisMap[MAX_ROOMS];

//	initializes rooms
int LGSRooms(CFILE *fp)
{
	int retval = LGS_OK;
	short i,p,highest_index;

	gs_ReadShort(fp, highest_index);
	if (highest_index != (short)Highest_room_index) 
	{
		Int3();
		return LGS_CORRUPTLEVEL;
	}
	
	short num_rooms;
	gs_ReadShort(fp,num_rooms);

	for (i = 0; i < num_rooms; i++)
	{
		gs_ReadByte(fp, AutomapVisMap[i]);
	}

	for (i = 0; i <= highest_index; i++)
	{		
				ubyte used;
		gs_ReadByte(fp, used);
		if (used) 
		{
		// reset lists
			Rooms[i].objects = -1;
			Rooms[i].vis_effects = -1;

		// we need to save some room info out.
			gs_ReadInt(fp, Rooms[i].flags);
			gs_ReadByte(fp,Rooms[i].pulse_time);
			gs_ReadByte(fp,Rooms[i].pulse_offset);
			gs_ReadVector(fp,Rooms[i].wind);
			gs_ReadFloat(fp, Rooms[i].last_render_time);
			gs_ReadFloat(fp, Rooms[i].fog_depth);
			gs_ReadFloat(fp, Rooms[i].fog_r);
			gs_ReadFloat(fp, Rooms[i].fog_g);
			gs_ReadFloat(fp, Rooms[i].fog_b);
			gs_ReadFloat(fp, Rooms[i].damage);
		//	gs_ReadInt(fp, Rooms[i].objects);
		//??	gs_WriteFloat(fp, Rooms[i].ambient_sound); // need to save an index of sounds.

		// Find out about texture changes
			//if (Gamesave_read_version>=1)
			//{
				int num_changed=cf_ReadShort (fp);

				for (int t=0;t<num_changed;t++)
				{
					int facenum=cf_ReadShort (fp);
					Rooms[i].faces[facenum].tmap=gs_Xlates->tex_handles[cf_ReadShort(fp)];
					Rooms[i].faces[facenum].flags |= FF_TEXTURE_CHANGED;
				}			
			//}
			for(p = 0;p<Rooms[i].num_portals;p++)
			{
				gs_ReadInt(fp, Rooms[i].portals[p].flags);
			}

			//load doorway info
			if (Rooms[i].flags & RF_DOOR) 
			{
				doorway *dp = Rooms[i].doorway_data;
				ASSERT(dp != NULL);
				gs_ReadByte(fp,  dp->state);
				gs_ReadByte(fp,  dp->flags);
				gs_ReadByte(fp,  dp->keys_needed);
				gs_ReadFloat(fp, dp->position);
				gs_ReadFloat(fp, dp->dest_pos);
				gs_ReadInt(fp,   dp->sound_handle);
				gs_ReadInt(fp,   dp->activenum);
				gs_ReadInt(fp,   dp->doornum);
			}

		}
	}

	//Rebuild the active doorway list
	//DoorwayRebuildActiveList();

	return retval;
}


//	loads in and sets these events
int LGSEvents(CFILE *fp)
{
	int retval = LGS_OK;

// clear events
	ClearAllEvents();	


	return retval;
}


//	loads in and sets these triggers
int LGSTriggers(CFILE *fp)
{
	short i, n_trigs=0;

	gs_ReadShort(fp, n_trigs);
	if (n_trigs != (short)Num_triggers) 
	{
		Int3();
		return LGS_CORRUPTLEVEL;
	}
	
	for (i = 0; i < n_trigs; i++)
	{

	// must free current trigger script if there's one allocated.
		FreeTriggerScript(&Triggers[i]);

		gs_ReadShort(fp, Triggers[i].flags);
		gs_ReadShort(fp, Triggers[i].activator);
	}

	return LGS_OK;
}


struct old_vis_attach_info
{
	int obj_handle,dest_objhandle;
	ubyte subnum,subnum2;

	ushort modelnum;
	ushort vertnum,end_vertnum;
};

struct old_vis_effect
{
	ubyte type;
	ubyte id;

	vector pos;

	vector velocity;
	float mass;
	float drag;
	float size;
	float lifeleft;
	float lifetime;
	float creation_time;

	int roomnum;
	ushort flags;
	int phys_flags;
	ubyte movement_type;
	short custom_handle;
	ushort lighting_color;

	vis_attach_info attach_info;
	axis_billboard_info billboard_info;
	
	vector end_pos;	

	short next;
	short prev;
};

void CopyVisStruct(vis_effect *vis,old_vis_effect *old_vis)
{
 	//Copy fields over
 	vis->type = old_vis->type;
 	vis->id = old_vis->id;
 	vis->pos = old_vis->pos;

 	vis->velocity = old_vis->velocity;
 	vis->mass = old_vis->mass;
 	vis->drag = old_vis->drag;
 	vis->size = old_vis->size;
 	vis->lifeleft = old_vis->lifeleft;
 	vis->lifetime = old_vis->lifetime;
 	vis->creation_time = old_vis->creation_time;

 	vis->roomnum = old_vis->roomnum;
 	vis->flags = old_vis->flags;
 	vis->phys_flags = old_vis->phys_flags;
 	vis->movement_type = old_vis->movement_type;
 	vis->custom_handle = old_vis->custom_handle;
 	vis->lighting_color = old_vis->lighting_color;

 	vis->attach_info.obj_handle = old_vis->attach_info.obj_handle;
 	vis->attach_info.dest_objhandle = old_vis->attach_info.dest_objhandle;
 	vis->attach_info.subnum = old_vis->attach_info.subnum;
 	vis->attach_info.subnum2 = old_vis->attach_info.subnum2;
 	vis->attach_info.modelnum = old_vis->attach_info.modelnum;
 	vis->attach_info.vertnum = old_vis->attach_info.vertnum;
 	vis->attach_info.end_vertnum = old_vis->attach_info.end_vertnum;

 	vis->billboard_info = old_vis->billboard_info;
 	vis->end_pos = old_vis->end_pos;	
}

// save viseffects
int LGSVisEffects(CFILE *fp)
{
	short i, count=0;

	gs_ReadShort(fp, count);

// reassemble viseffect list.
	for (i = 0; i < count; i++)
	{
		int roomnum, v;
		vis_effect vis;

		cf_ReadBytes((ubyte *)&vis, sizeof(vis_effect), fp);
		roomnum = vis.roomnum;

		//Check for old struct, and if found, fix it
		if ((vis.type != VIS_FIREBALL) || (roomnum == -1) || (!ROOMNUM_OUTSIDE(roomnum) && ((roomnum > Highest_room_index) || !Rooms[roomnum].used)) || ((ROOMNUM_OUTSIDE(roomnum) && (CELLNUM(roomnum) > 65535)))) {
		 	old_vis_effect old_vis;

			//Copy new into old
		 	memcpy((ubyte *) &old_vis, (ubyte *) &vis, sizeof(old_vis_effect));

			//Read extra data from old
			cf_ReadBytes(((ubyte *)&old_vis) + sizeof(vis_effect), sizeof(old_vis_effect)-sizeof(vis_effect), fp);

			//Copy from old to new struct
			CopyVisStruct(&vis,&old_vis);

			//Reset new room number
			roomnum = vis.roomnum;
		}

		vis.roomnum = vis.prev = vis.next = -1;
		v = VisEffectAllocate();
		if(v >= 0) 
		{
			//DAJ -1FIX
			memcpy(&VisEffects[v], &vis, sizeof(vis_effect));
			VisEffectLink(v, roomnum);
		}
	}

	return LGS_OK;
}

// players
int LGSPlayers(CFILE *fp)
{
	player *plr = &Players[0];
	short size;


// must do this if we read player struct as whole.
	plr->inventory.Reset(false,INVRESET_ALL);
	plr->counter_measures.Reset(false,INVRESET_ALL);

	gs_ReadShort(fp, size);
	if (size != sizeof(player)) 
	{
		Int3();
		return LGS_OUTDATEDVER;
	}
	else 
	{
		int guided_handle;
		cf_ReadBytes((ubyte *)plr, sizeof(player), fp);
		if (plr->guided_obj) 
		{
			gs_ReadInt(fp, guided_handle);
			plr->guided_obj = &Objects[guided_handle&HANDLE_OBJNUM_MASK];
		}
	// save inventory and countermeasures
	// must do this if we read player struct as whole.
		plr->inventory.ReadInventory(fp);
		plr->counter_measures.ReadInventory(fp);
	}

	int ship_index = Players[Player_num].ship_index;

	if (ship_index<0)
		ship_index=0;
	
	FreeCockpit();
	CloseShipHUD();
	InitShipHUD(ship_index);							
	InitCockpit(ship_index);

	return LGS_OK;
}

extern int Physics_NumLinked;
extern int PhysicsLinkList[MAX_OBJECTS];

int inreadobj=0;

void VerifySaveGame(CFILE *fp)
{
	int testint;
	testint=cf_ReadInt(fp);
	ASSERT(testint==0xF00D4B0B);

}

#define VERIFY_SAVEFILE	
//VerifySaveGame(fp)
extern char MarkerMessages[MAX_PLAYERS*2][MAX_MARKER_MESSAGE_LENGTH];
extern int Marker_message;
//	loads in and sets these objects
int LGSObjects(CFILE *fp, int version)
{
	inreadobj = 1;
	int retval = LGS_OK;
	vector pos, last_pos;
	int roomnum;
	int i, j, highest_index;
	int max_terr;

	matrix *objmat = (matrix *) mem_malloc(sizeof(*objmat) * MAX_OBJECTS);

	Osiris_DisableCreateEvents();
// we must reset some data before continuing.
	InitBigObjects();

START_VERIFY_SAVEFILE(fp);
	Marker_message = cf_ReadInt(fp);
	int num_marker_msgs = cf_ReadShort(fp);
	for(i=0;i<num_marker_msgs;i++)
	{
		int msg_len = cf_ReadShort(fp);
		cf_ReadBytes((ubyte *)MarkerMessages[i],msg_len,fp);		
	}

	highest_index = (int)cf_ReadShort(fp);
	
	//load what objects are stuck to each other
	int max_num_linked = cf_ReadInt(fp);
	Physics_NumLinked = cf_ReadInt(fp);
	for (i = 0; i < max_num_linked; i++)
	{
		PhysicsLinkList[i] = cf_ReadInt(fp);
	}

	// load AI information
	//////////////////////////////////
	int num_read_max_dynamic_paths = cf_ReadInt(fp);
	int num_read_max_nodes = cf_ReadInt(fp);

	int num_dp_to_read = min(MAX_DYNAMIC_PATHS,num_read_max_dynamic_paths);
	int num_dp_to_skip = (MAX_DYNAMIC_PATHS<num_read_max_dynamic_paths)?num_read_max_dynamic_paths-MAX_DYNAMIC_PATHS:0;

	int num_n_to_read = min(MAX_NODES,num_read_max_nodes);
	int num_n_to_skip = (MAX_NODES<num_read_max_nodes)?num_read_max_nodes-MAX_NODES:0;

	int s;
	for(i=0;i<num_dp_to_read;i++)
	{
		AIDynamicPath[i].num_nodes = cf_ReadShort(fp);
		AIDynamicPath[i].use_count = cf_ReadShort(fp);
		AIDynamicPath[i].owner_handle = cf_ReadInt(fp);

		for(s=0;s<num_n_to_read;s++)
		{
			AIDynamicPath[i].pos[s].x = cf_ReadFloat(fp);
			AIDynamicPath[i].pos[s].y = cf_ReadFloat(fp);
			AIDynamicPath[i].pos[s].z = cf_ReadFloat(fp);
			AIDynamicPath[i].roomnum[s] = cf_ReadInt(fp);
		}

		for(s=0;s<num_n_to_skip;s++)
		{
			cf_ReadFloat(fp);
			cf_ReadFloat(fp);
			cf_ReadFloat(fp);
			cf_ReadInt(fp);
		}
	}

	for(i=0;i<num_dp_to_skip;i++)
	{
		cf_ReadShort(fp);
		cf_ReadShort(fp);
		cf_ReadInt(fp);

		for(s=0;s<num_n_to_read;s++)
		{
			cf_ReadFloat(fp);
			cf_ReadFloat(fp);
			cf_ReadFloat(fp);
			cf_ReadInt(fp);
		}

		for(s=0;s<num_n_to_skip;s++)
		{
			cf_ReadFloat(fp);
			cf_ReadFloat(fp);
			cf_ReadFloat(fp);
			cf_ReadInt(fp);
		}
	}

	int num_read_rooms = cf_ReadInt(fp);
	int num_r_to_read = min(MAX_ROOMS,num_read_rooms);
	int num_r_to_skip = (MAX_ROOMS<num_read_rooms)?num_read_rooms-MAX_ROOMS:0;

	AIAltPathNumNodes = cf_ReadInt(fp);
	for(i=0;i<num_r_to_read;i++)
	{
		AIAltPath[i] = cf_ReadInt(fp);
	}
	
	for(i=0;i<num_r_to_skip;i++)
	{
		cf_ReadInt(fp);
	}

	//////////////////////////////////
	
	
	Osiris_DisableEvents(OEM_OBJECTS|OEM_TRIGGERS|OEM_LEVELS);
	for (i = 0; i < MAX_OBJECTS; i++)
	{
		if(Objects[i].lighting_render_type!=LRT_LIGHTMAPS)
		{
			if (Objects[i].type != OBJ_NONE) 
			{
				Objects[i].next = -1;
				Objects[i].prev = -1;
				ObjDelete(i);
			}
		}
		else
		{
			Objects[i].next = -1;
			Objects[i].prev = -1;
			ObjUnlink(i);
		}

	}
	Osiris_EnableEvents(OEM_OBJECTS|OEM_TRIGGERS|OEM_LEVELS);
	for (i = 0; i <= highest_index; i++)
	{
		object *op;
		weapon *wpn;
		ship *shp;
		object_info *obji;
		door *door;
		int index, nattach, new_model, handle;
		ubyte type, dummy_type;
		short sindex, size;


	//	if((i==98)||(i==100))
	//		Int3();
		int sig;
		gs_ReadInt(fp,sig);

		ASSERT(sig==0xBADB0B);
		
		gs_ReadByte(fp, type);
		

		// skip rooms and deleted objects.  if this slot currently contains a real object, it should be
		// destroyed (unlinked, deallocated, etc) to match the new object list.
		if (type == OBJ_NONE) 
		{
			Osiris_DisableEvents(OEM_OBJECTS|OEM_TRIGGERS|OEM_LEVELS);
			if(Objects[i].type != OBJ_NONE) 
				ObjDelete(i);
			Osiris_EnableEvents(OEM_OBJECTS|OEM_TRIGGERS|OEM_LEVELS);
			continue;
		}
		ubyte l_rend_type;
		gs_ReadByte(fp, l_rend_type);
		
		//See if the object has changed from it's original lightmap type
		//DAJ added check for type
		if( (Objects[i].type != OBJ_NONE) && (Objects[i].lighting_render_type==LRT_LIGHTMAPS) && (l_rend_type!=LRT_LIGHTMAPS) )
		{
			Osiris_DisableEvents(OEM_OBJECTS|OEM_TRIGGERS|OEM_LEVELS);
			ObjDelete(i);
			Osiris_EnableEvents(OEM_OBJECTS|OEM_TRIGGERS|OEM_LEVELS);
		}
		
		op = &Objects[i];
		op->lighting_render_type = l_rend_type;

		
		//Store whether or not we have a pointer to lighting_info
		ubyte has_lightinfo;
		gs_ReadByte(fp,has_lightinfo);
		if(has_lightinfo)
		{
			if(!op->lighting_info)
			{
				op->lighting_info = (light_info *) mem_malloc(sizeof(*op->lighting_info));
			}
			cf_ReadBytes((ubyte *)op->lighting_info,sizeof(*op->lighting_info),fp);
		}
		
		
		// validate handle.
		gs_ReadInt(fp, handle);
		if ((handle & HANDLE_OBJNUM_MASK) != i) {
			Int3();
			retval = LGS_OBJECTSCORRUPT;
			goto done;
		}

		// read in the rest.
		gs_ReadByte(fp, dummy_type);

		//	positional information (not assigned to object 'cause of Unlink)
		gs_ReadInt(fp, roomnum);
		gs_ReadVector(fp, pos);
		gs_ReadVector(fp, last_pos);
		gs_ReadMatrix(fp, objmat[i]);

		// object name
		gs_ReadByte(fp,j);
		if(j>0){
			if(op->name)
				mem_free(op->name);
			op->name = (char *)mem_malloc(j+1);
			if(!op->name)
				Error("Out of memory");

			cf_ReadBytes((ubyte *)op->name,j,fp);
			op->name[j] = '\0';
		}else{
			op->name = NULL;
		}		

//	Kill any script thread associated with this object.
		

		if (op->type != OBJ_NONE) 
		{
			if(Objects[i].lighting_render_type!=LRT_LIGHTMAPS)
				FreeObjectScripts(op,false);

		// Free up effects memory
			if (op->effect_info) 
			{
				mem_free (op->effect_info);
				op->effect_info=NULL;
			}
			if (op->ai_info != NULL) 
			{
				AIDestroyObj(op);
				mem_free(op->ai_info);
				op->ai_info = NULL;
			}
			if (op->dynamic_wb != NULL) 
			{
				mem_free(op->dynamic_wb);
				op->dynamic_wb = NULL;
			}
			if (op->attach_children != NULL) 
			{
				mem_free(op->attach_children);
				op->attach_children = NULL;
			}
			
		}
		op->type = type;
		op->handle = handle;
		op->dummy_type = dummy_type;
		
		op->roomnum = roomnum;			
		op->pos = pos;
		op->last_pos = last_pos;

	//	data universal to all objects
		gs_ReadShort(fp, op->id);

		//Get type, and xlate if dummy
		if (type == OBJ_DUMMY)
			type = op->dummy_type;

	// xlate id to new id.
		switch (type) 
		{
		case OBJ_ROBOT:
		case OBJ_POWERUP:
		case OBJ_BUILDING:
		case OBJ_CLUTTER:
			op->id = gs_Xlates->obji_indices[op->id]; 
			obji = &Object_info[op->id];
			break;
		case OBJ_DOOR:
			op->id = gs_Xlates->door_handles[op->id];
			door = &Doors[op->id];
			break;
		case OBJ_WEAPON:
			op->id = gs_Xlates->wpn_handles[op->id];
			wpn = &Weapons[op->id];
			break;
		case OBJ_PLAYER:
			shp = &Ships[Players[op->id].ship_index];
			break;
		}

		gs_ReadInt(fp, op->flags);

		op->flags|=OF_SERVER_OBJECT;

		gs_ReadByte(fp, op->control_type);
		gs_ReadByte(fp, op->movement_type);
		gs_ReadByte(fp, op->render_type);
		
		gs_ReadShort(fp, op->renderframe);
		gs_ReadFloat(fp, op->size);
		gs_ReadFloat(fp, op->shields);
		gs_ReadByte(fp, op->contains_type);
		gs_ReadByte(fp, op->contains_id);
		gs_ReadByte(fp, op->contains_count);
		gs_ReadFloat(fp, op->creation_time);
		gs_ReadFloat(fp, op->lifeleft);
		gs_ReadFloat(fp, op->lifetime);
		gs_ReadInt(fp, op->parent_handle);

	// attachment info. free old info and load in new.
		gs_ReadInt(fp, op->attach_ultimate_handle);
		gs_ReadInt(fp, op->attach_parent_handle);
		
		gs_ReadInt(fp, nattach);
		if (nattach) 
		{
			int f_allocated;

			if(version >= 2)
				gs_ReadInt(fp, f_allocated);
			else
				f_allocated = 1;

			if(f_allocated)
			{
				//mprintf((0,"Object %d has %d attach points.\n",i,nattach));
				op->attach_children = (int *)mem_malloc(sizeof(int)*nattach);
				for (j = 0; j < nattach; j++)
					gs_ReadInt(fp, op->attach_children[j]);
			}
		}

		VERIFY_SAVEFILE;

		gs_ReadByte(fp, op->attach_type);
		gs_ReadShort(fp, op->attach_index); 
		gs_ReadFloat(fp, op->attach_dist); 
		gs_ReadVector(fp, op->min_xyz);
		gs_ReadVector(fp, op->max_xyz);
		gs_ReadFloat(fp, op->impact_size);
		gs_ReadFloat(fp, op->impact_time);
		gs_ReadFloat(fp, op->impact_player_damage);
		gs_ReadFloat(fp, op->impact_generic_damage);
		gs_ReadFloat(fp, op->impact_force);

	// custom default script info
		gs_ReadByte(fp,j);
		if(j>0){
			op->custom_default_script_name = (char *)mem_malloc(j+1);
			if(!op->custom_default_script_name)
				Error("Out of memory");

			cf_ReadBytes((ubyte *)op->custom_default_script_name,j,fp);
			op->custom_default_script_name[j] = '\0';
		}else{
			op->custom_default_script_name = NULL;
		}

		gs_ReadByte(fp,j);
		if(j>0){
			op->custom_default_module_name = (char *)mem_malloc(j+1);
			if(!op->custom_default_module_name)
				Error("Out of memory");

			cf_ReadBytes((ubyte *)op->custom_default_module_name,j,fp);
			op->custom_default_module_name[j] = '\0';
		}else{
			op->custom_default_module_name = NULL;
		}
		
		VERIFY_SAVEFILE;

		gs_ReadShort(fp, op->position_counter);
		
		VERIFY_SAVEFILE;

//	write out all structures here.
	// movement info.
		gs_ReadShort(fp, size);
		if (size != sizeof(op->mtype)) 
		{
			Int3();
			retval = LGS_OUTDATEDVER;
			goto done;
		}
		cf_ReadBytes((ubyte *)&op->mtype, size, fp);

		VERIFY_SAVEFILE;
	//Control info, determined by CONTROL_TYPE 
		gs_ReadShort(fp, size);
		if (size != sizeof(op->ctype)) 
		{
			Int3();
			retval = LGS_OUTDATEDVER;
			goto done;
		}
		cf_ReadBytes((ubyte *)&op->ctype, size, fp);

		VERIFY_SAVEFILE;
		// remap bitmap handle if this is a fireball!
  		if (type == OBJ_FIREBALL) 
		{
			index = op->ctype.blast_info.bm_handle;
			op->ctype.blast_info.bm_handle = (index > -1) ? gs_Xlates->bm_handles[index] : -1;
		}

		
		// save ai information.   
		retval = LGSObjAI(fp, &op->ai_info);
		if (retval != LGS_OK)
			goto lgsobjs_fail;


		VERIFY_SAVEFILE;


		// save out rendering information
		gs_ReadShort(fp, size);
		if (size != sizeof(op->rtype)) 
		{
			Int3();
			retval = LGS_OUTDATEDVER;
			goto done;
		}
		cf_ReadBytes((ubyte *)&op->rtype, size, fp);

		op->size = cf_ReadFloat(fp);
		
	// page in correct graphical data now.
		switch (op->render_type)
		{
		case RT_NONE:
		case RT_EDITOR_SPHERE:
		case RT_FIREBALL:	
		case RT_LINE:			
		case RT_PARTICLE:
		case RT_SPLINTER:
		case RT_ROOM:			
			break;
		case RT_WEAPON:			
			if (!(op->flags & OF_POLYGON_OBJECT)) 
			{
				if (!(Weapons[op->id].flags&WF_IMAGE_BITMAP))
				{
					PageInVClip (Weapons[op->id].fire_image_handle);
				}
				break;
			}
			break;
		case RT_POLYOBJ:					// be sure to use translated handles for polyobjs and textures
			{	
			// the paging mess.  must update size of object accordingly.
				sindex = (short)op->rtype.pobj_info.model_num;
				new_model = (sindex > -1) ? gs_Xlates->model_handles[sindex] : -1;
				if( (new_model != op->rtype.pobj_info.model_num) || (Poly_models[new_model].flags & PMF_NOT_RESIDENT))
				{
					switch (type) 
					{
					case OBJ_DOOR: 
						PageInPolymodel(new_model); 	
						ComputeDefaultSize(OBJ_DOOR, new_model, &op->size);
						break;
					case OBJ_ROBOT:
					case OBJ_POWERUP:
					case OBJ_BUILDING:
					case OBJ_CLUTTER: PageInPolymodel(new_model, type, &obji->size); op->size = obji->size; break;
					case OBJ_WEAPON: PageInPolymodel(new_model, OBJ_WEAPON, &wpn->size); op->size = wpn->size; break;
					case OBJ_PLAYER: PageInPolymodel (new_model, OBJ_PLAYER, &shp->size); op->size = shp->size; break;
					case OBJ_ROOM: op->size = ComputeRoomBoundingSphere(&pos,&Rooms[op->id]);
						break;

					default: PageInPolymodel(new_model);
					}
				}
				op->rtype.pobj_info.model_num = new_model;

				sindex = (short)op->rtype.pobj_info.dying_model_num;
				new_model = (sindex > -1) ? gs_Xlates->model_handles[sindex] : -1;
				if (new_model != op->rtype.pobj_info.dying_model_num)
				{
					switch (type)
					{
					case OBJ_PLAYER: PageInPolymodel (new_model, OBJ_PLAYER, &shp->size); op->size = shp->size; break;
					}
				}
				op->rtype.pobj_info.dying_model_num = new_model;
		
				index = op->rtype.pobj_info.tmap_override;
				op->rtype.pobj_info.tmap_override = (index>-1) ? gs_Xlates->tex_handles[index] : -1;
				
				memset(&op->rtype.pobj_info.multi_turret_info, 0, sizeof(multi_turret));
				poly_model *pm=&Poly_models[op->rtype.pobj_info.model_num];
				
				if(pm->n_attach)
				{
					mprintf((0,"*Object %d has %d attach points.\n",i,pm->n_attach));
				}
				
				polyobj_info *p_info = &op->rtype.pobj_info;
				int num_wbs = pm->num_wbs;
				int count = 0;
				for(int j = 0; j < num_wbs; j++)
				{
					ASSERT(pm->poly_wb[j].num_turrets >= 0 && pm->poly_wb[j].num_turrets <= 6400);
					count += pm->poly_wb[j].num_turrets;
				}

				p_info->multi_turret_info.num_turrets = count;

				if((count > 0) && (p_info->multi_turret_info.keyframes == NULL))
				{
					int cur = 0;

					p_info->multi_turret_info.time = 0;
					p_info->multi_turret_info.keyframes = (float *) mem_malloc(sizeof(float) * count);
					p_info->multi_turret_info.last_keyframes = (float *) mem_malloc(sizeof(float) * count);
					p_info->multi_turret_info.flags = 0;
				}
				//Do Animation stuff
				custom_anim multi_anim_info;
				cf_ReadBytes((ubyte *)&multi_anim_info,sizeof(multi_anim_info),fp);
				ObjSetAnimUpdate(i, &multi_anim_info);
				break;
			}

		case RT_SHARD:
			sindex = (short)op->rtype.shard_info.tmap;
			op->rtype.shard_info.tmap = (sindex>-1) ? gs_Xlates->tex_handles[sindex] : -1;
			break;

		default:
			Int3();
		}
		
		VERIFY_SAVEFILE;

	//!! dynamic weapon battery info!!
		retval = LGSObjWB(fp, op);
		if (retval != LGS_OK)
			goto lgsobjs_fail;

		VERIFY_SAVEFILE;

	// save effect info!
		retval = LGSObjEffects(fp, op);
		if (retval != LGS_OK)
			goto lgsobjs_fail;

		VERIFY_SAVEFILE;

	//save script stuff.
		if(Objects[i].lighting_render_type!=LRT_LIGHTMAPS)
		{
			//Don't init the scripts for an object with lightmaps, because it was never destroyed!
			InitObjectScripts (op);		
		}

	//	special case saves
		retval = LGSObjSpecial(fp, op);
		if (retval != LGS_OK)
			goto lgsobjs_fail;

	// link into mine.
	// turn off big object flags if its a big object. (ObjLink will take care of this.)
		if (op->flags & OF_BIG_OBJECT) 
			op->flags &= (~OF_BIG_OBJECT);

		op->roomnum = roomnum;//-1;
		//ObjLink(OBJNUM(op), roomnum);
		//ObjSetOrient(op, &objmat);

	}
	for (;i < MAX_OBJECTS; i++)
	{
		if (Objects[i].type != OBJ_NONE)
		{
			Osiris_DisableEvents(OEM_OBJECTS|OEM_TRIGGERS|OEM_LEVELS);
			ObjDelete(i);
			Osiris_EnableEvents(OEM_OBJECTS|OEM_TRIGGERS|OEM_LEVELS);
		}
	}

	for (i = 0; i < MAX_OBJECTS; i++)
	{
		Objects[i].next = -1;
		Objects[i].prev = -1;
	}
	for(i=0;i<MAX_ROOMS;i++)
	{
		Rooms[i].objects = -1;
	}
	max_terr = TERRAIN_WIDTH*TERRAIN_DEPTH;
	for(i=0;i<max_terr;i++)
	{
		Terrain_seg[i].objects = -1;
	}
	inreadobj=0;
	Highest_object_index = highest_index;
	
	//CreateRoomObjects();
	
	for (i=0;i <= Highest_object_index; i++)
	{
		object *op = &Objects[i];
		if(Objects[i].type!=OBJ_NONE)
		{
			int newroom = op->roomnum;
			op->roomnum=-1;
			ObjLink(OBJNUM(op), newroom);
			ObjSetOrient(op, &objmat[i]);
			if(op->type==OBJ_ROOM)
			{
				mprintf((0,"Object %d is a room and Is%s a big object. Size=%f\n",i,(op->flags&OF_BIG_OBJECT)?"":"n't",op->size));
				if((op->size >= ((TERRAIN_SIZE*(float)1))) && !(op->flags & OF_BIG_OBJECT))
				{
					BigObjAdd(i);
				}
				
				ObjSetAABB(op);
			}
			
		}
		/*
		if((op->attach_ultimate_handle)&&(OBJECT_HANDLE_NONE!=op->attach_ultimate_handle))
		{
			mprintf((0,"Object %d has an ultimate parent of %d (%d)\n",i,OBJNUM(ObjGet(op->attach_ultimate_handle)),op->attach_parent_handle ));
			ASSERT(op->flags&OF_ATTACHED);
		}
		if((op->attach_ultimate_handle)&&(OBJECT_HANDLE_NONE!=op->attach_parent_handle))
		{
			mprintf((0,"Object %d has a parent of %d (%d)\n",i,OBJNUM(ObjGet(op->attach_parent_handle)),op->attach_parent_handle ));
			ASSERT(op->flags&OF_ATTACHED);
		}
		*/
		
	}
	mprintf((0,"Objects[121].prev=%d\n",Objects[121].prev));
	ResetFreeObjects();
	mprintf((0, "highest obj index = %d, ", Highest_object_index));
	ObjReInitPositionHistory();
	

END_VERIFY_SAVEFILE(fp, "Objects load");
lgsobjs_fail:
	
	Osiris_EnableCreateEvents();

done:;

	mem_free(objmat);

	return retval;
}


//	loads ai
int LGSObjAI(CFILE *fp, ai_frame **pai)
{
	ai_frame *ai;
	short size;
	sbyte read_ai;

	*pai = NULL;

	gs_ReadByte(fp, read_ai);
	if (!read_ai) 
		return LGS_OK;

	gs_ReadShort(fp, size);
	if (size != sizeof(ai_frame)) 
		return LGS_OUTDATEDVER;

	*pai = (ai_frame *)mem_malloc(size);
	ai= *pai;

	cf_ReadBytes((ubyte *)ai, size, fp);

	return LGS_OK;
}



//	loads fx
int LGSObjEffects(CFILE *fp, object *op)
{
	short size;
	sbyte do_read;

 	op->effect_info = NULL;

	gs_ReadByte(fp,do_read); 
	if (do_read) {
		gs_ReadShort(fp, size);
		if (size != sizeof(effect_info_s)) 
			return LGS_OUTDATEDVER;
		
		op->effect_info = (effect_info_s *)mem_malloc(size);
		effect_info_s *ei = op->effect_info;

		cf_ReadBytes((ubyte *)ei, size, fp);
	}

	return LGS_OK;
}

//	loads fx
int LGSObjWB(CFILE *fp, object *op)
{
	dynamic_wb_info *dwba = NULL;
	int i;
	sbyte num_wbs;

	gs_ReadByte(fp, num_wbs);
	if (!num_wbs) 
		return LGS_OK;

	dwba = (dynamic_wb_info *)mem_malloc(sizeof(dynamic_wb_info)*num_wbs);

	for (i = 0; i< num_wbs; i++)
	{
		dynamic_wb_info *dwb = &dwba[i];
		cf_ReadBytes((ubyte *)dwb, sizeof(dynamic_wb_info), fp);
	}
	op->dynamic_wb = dwba;

	return LGS_OK;
}


// loads special object info
int LGSObjSpecial(CFILE *fp, object *op)
{
	int retval = LGS_OK;

	return retval;
}


// load spew
int LGSSpew(CFILE *fp)
{
	int count=0;

// read GLOBAL value 
	gs_ReadShort(fp, spew_count);

	for (int i = 0; i < MAX_SPEW_EFFECTS; i++)
	{
		ubyte used;
		gs_ReadByte(fp, used);
		if (used) 
			cf_ReadBytes((ubyte *)&SpewEffects[i], sizeof(spewinfo), fp);
	}

	return LGS_OK;
}

int LGSMatcens(CFILE *fp)
{
	int num_matcens = cf_ReadInt(fp);

	for(int i = 0; i < num_matcens; i++)
	{
		Matcen[i]->LoadData(fp);
	}

	return LGS_OK;
}


int LGSSnapshot(CFILE *fp)
{
	int bm_handle = -1;
	sbyte valid_snapshot=0;

// get snapshot byte
	valid_snapshot = cf_ReadByte(fp);
	
//	if valid, read it in, otherwise just return
	if (valid_snapshot)
		bm_handle = bm_AllocLoadBitmap(fp, 0);

	return bm_handle;
}
