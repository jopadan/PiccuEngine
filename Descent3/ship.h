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

#ifndef SHIP_H
#define SHIP_H

#include "pstypes.h"
#include "manage.h"
#include "object.h"
#include "robotfirestruct.h"
#include "player.h"

#define MAX_SHIPS				30

//#ifdef DEMO //Demo2 will use GL
//#define DEFAULT_SHIP "Pyro-SE"
//#else
#define DEFAULT_SHIP "Pyro-GL"
//#endif

// Ship fire flags
#define SFF_FUSION	1		// fires like fusion
#define SFF_ZOOM	4		// Zooms in
#define SFF_TENTHS	8		// Ammo displays in tenths

// Default ship IDs
#define SHIP_PYRO_ID	0
#define SHIP_PHOENIX_ID	1
#define SHIP_MAGNUM_ID	2

#define MAX_DEFAULT_SHIPS	3

// Ship flags
#define SF_DEFAULT_ALLOW	1	//Allowed by default

struct ship
{
	char name[PAGENAME_LEN];
	float size;
	physics_info	phys_info;		//the physics data for this obj type.
	int model_handle;					//  a polygon model
	int dying_model_handle;			// Dying polygon model

	int	med_render_handle;	//handle for med res version of this object
	int	lo_render_handle;		//handle for lo res version of this object   

	float med_lod_distance;
	float lo_lod_distance;

	otype_wb_info static_wb[MAX_PLAYER_WEAPONS];
	ubyte fire_flags[MAX_PLAYER_WEAPONS];		// how a particular weapon fires
	int	max_ammo[MAX_PLAYER_WEAPONS];

	int	firing_sound[MAX_PLAYER_WEAPONS];			//sound the weapon makes while button held down
	int	firing_release_sound[MAX_PLAYER_WEAPONS];	//sound the weapon makes when the button is released

	int	spew_powerup[MAX_PLAYER_WEAPONS];	//which powerup to spew for each weapon

	char cockpit_name[PAGENAME_LEN];		// name of cockpit.inf file 
	char hud_config_name[PAGENAME_LEN];	// name of hud configuration file

	float armor_scalar;

	int flags;
	ubyte used;
};

extern int Num_ships;
extern ship Ships[MAX_SHIPS];

extern char *AllowedShips[];

// Sets all ships to unused
void InitShips ();

// Allocs a ship for use, returns -1 if error, else index on success
int AllocShip ();

// Frees ship index n
void FreeShip (int n);

// Gets next ship from n that has actually been alloced
int GetNextShip (int n);

// Gets previous ship from n that has actually been alloced
int GetPrevShip (int n);

// Searches thru all ships for a specific name, returns -1 if not found
// or index of ship with name
int FindShipName (char *name);

// Given a filename, loads either the model or vclip found in that file.  If type
// is not NULL, sets it to 1 if file is model, otherwise sets it to zero
int LoadShipImage (char *filename);

// Given a ship handle, returns that ships image for framenum
int GetShipImage (int handle);

// This is a very confusing function.  It takes all the ships that we have loaded 
// and remaps then into their proper places (if they are static). 
void RemapShips ();

// goes thru every entity that could possible have a ship index (ie objects, ships, etc)
// and changes the old index to the new index
void RemapAllShipObjects (int old_index,int new_index);

#endif
