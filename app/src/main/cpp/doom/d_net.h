#ifndef __D_NET__
#define __D_NET__

#include "doomtype.h"
#include "d_ticcmd.h"

/* Single-player only: the network layer was removed. */
#define BACKUPTICS	12

void D_CheckNetGame(void);
void D_QuitNetGame(void);
void NetUpdate(void);
void TryRunTics(void);

#endif
