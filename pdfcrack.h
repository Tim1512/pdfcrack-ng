/**
 * Copyright (C) 2006 Henning Norén
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#ifndef _PDFCRACK_H_
#define _PDFCRACK_H_

#include <stdio.h>
#include "common.h"

bool
initPDFCrack(const EncData *e, const uint8_t *upw, const bool user,
		const char *wl, const passwordMethod pm, FILE *file,
		const char *cs, const unsigned int minPw,
		const unsigned int maxPw, const bool perm, const int nrOfThreadsLoc,
		const int zone_local, const int nrOfZones_local);

bool
loadState(FILE *file, EncData *e, char **wl, bool *user);

void
saveState(FILE *file);

void
cleanPDFCrack(void);

bool
runCrackRev2(void);

bool
runCrackRev2_o(void);

bool
runCrackRev2_of(void);

bool
runCrackRev3(void);

void
crackRev3Thread(void*);

void
crackRev2Thread(void*);

bool
runCrackRev3MultiThreads(void);

bool
runCrackRev2MultiThreads(void);

bool
runCrackMultiThreads(void);


bool
runCrackRev3_o(void);

bool
runCrackRev3_of(void);

void
runCrack(void);

bool
printProgress(void);

unsigned int
getNrProcessed(void);

void
setPwdMultiThread(int *password, const uint8_t *charset, unsigned int charsetLen);

int getNrOfThreads(void);

#endif /** _PDFCRACK_H_ */

