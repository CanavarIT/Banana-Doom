// Single-player game tic scheduler (network code removed).

#include <stdio.h>
#include "m_menu.h"
#include "i_system.h"
#include "i_video.h"
#include "g_game.h"
#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"

ticcmd_t	netcmds[MAXPLAYERS][BACKUPTICS];

int		maketic;
int		ticdup = 1;

void G_BuildTiccmd(ticcmd_t *cmd);
void D_ProcessEvents(void);
void D_DoAdvanceDemo(void);
extern boolean	advancedemo;

static int gametime;

//
// NetUpdate: builds ticcmds for the elapsed real time.
//
void NetUpdate(void)
{
    int nowtime = I_GetTime();
    int newtics = nowtime - gametime;
    int i;

    gametime = nowtime;
    if (newtics <= 0)
	return;

    for (i = 0; i < newtics; i++)
    {
	I_StartTic();
	D_ProcessEvents();
	if (maketic - gametic >= BACKUPTICS/2 - 1)
	    break;
	G_BuildTiccmd(&netcmds[consoleplayer][maketic % BACKUPTICS]);
	maketic++;
    }
}

void D_CheckNetGame(void)
{
    int i;

    netgame = false;
    ticdup = 1;
    consoleplayer = displayplayer = 0;
    for (i = 0; i < MAXPLAYERS; i++)
	playeringame[i] = (i == 0);
}

void D_QuitNetGame(void)
{
}

//
// TryRunTics: waits (sleeping, not spinning) for at least one tic and runs
// all the tics that are due.
//
void TryRunTics(void)
{
    static int oldentertics;
    int entertic = I_GetTime();
    int realtics = entertic - oldentertics;
    int availabletics, counts;

    oldentertics = entertic;
    NetUpdate();

    availabletics = maketic - gametic;
    if (realtics < availabletics - 1)
	counts = realtics + 1;
    else if (realtics < availabletics)
	counts = realtics;
    else
	counts = availabletics;
    if (counts < 1)
	counts = 1;

    while (maketic < gametic + counts)
    {
	I_Sleep(1);
	NetUpdate();
	if (I_GetTime() - entertic >= 20)
	{
	    M_Ticker();
	    return;
	}
    }

    while (counts--)
    {
	if (advancedemo)
	    D_DoAdvanceDemo();
	M_Ticker();
	G_Ticker();
	gametic++;
	NetUpdate();
    }
}
