/**
 * Copyright (C) 2006-2008 Henning Norén
 * Copyright (C) 1996-2005 Glyph & Cog, LLC.
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
 *
 * Copyright (C) 2009 Andreas Meier, Michael Kuhn
 * Multi-core (threads) and multi-computer (zones) extension
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "pdfcrack.h"
#include "md5.h"
#include "rc4.h"
#include "passwords.h"

#include <pthread.h>

/** sets the number of bytes to decrypt for partial test in revision 3.
    Three should be a good number for this as this mean a match should only
    happen every 256^3=16777216 check and that should be unique enough to
    motivate a full retry on that entry.
 */
#define PARTIAL_TEST_SIZE 3

static const uint8_t
pad[32] = {
		0x28,0xBF,0x4E,0x5E,0x4E,0x75,0x8A,0x41,
		0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,
		0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,
		0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A
};

/** buffers for stuff that we can precompute before the actual cracking */
static uint8_t *encKeyWorkSpace;
static uint8_t password_user[33];
static uint8_t *rev3TestKey;
static unsigned int ekwlen;

/** points to the current password in clear-text */
static uint8_t *currPW;
/** current length of the password we are working with */
static unsigned int currPWLen;

/** statistics */
static unsigned int nrprocessed;
static time_t startTime;

/** pointer to the actual encoding-data from the pdf */
static const EncData *encdata;

/** some configuration switches */
static bool crackDone;
static bool knownPassword;
static bool workWithUser;
static int nrOfThreads;

pthread_mutex_t lock;
bool endOfSearch;
bool pwFound;

// used for savestate
long globalGenPwdCnt; // counts the total number of generated passwords (=> basically a pwd id)
long* generatedPWCounts; // array containing a value for each threads
                         // (stores the 'pwd id' (see above) of the pwd currently being processed)
unsigned char** nonPermutatedPwds; // we save the last non-permutated password in savestate.


///* debug output */
//static void printCurrPw() {
//	unsigned char tmp[33];
//	memcpy(tmp, currPW, 32);
//	tmp[currPWLen] = 0;
//	printf("currPW: %s \n", tmp);
//}

/** Print out some statistics */
bool
printProgress(void) {
	time_t currentTime;
	char str[33];

	if(crackDone)
		return true;

	currentTime = time(NULL);
	memcpy(str,currPW,currPWLen);
	str[currPWLen] = '\0';
	printf("Average Speed: %.1f w/s. ",
			nrprocessed/difftime(currentTime,startTime));
	printf("Current Word: '%s'\n",str);
	fflush(stdout);
	nrprocessed = 0;
	startTime = time(NULL);
	return false;
}

/**
 * Initialisation of the encryption key workspace to manage a bit faster
 * switching between keys
 */
static unsigned int
initEncKeyWorkSpace(const int revision, const bool encMetaData,
		const int permissions, const uint8_t *ownerkey,
		const uint8_t *fileID, const unsigned int fileIDLen) {
	/**
	 *   Algorithm 3.2 Computing an encryption key (PDF Reference, v 1.7, p.125)
	 *
	 *   Make space for:
	 *   field           | bytes
	 *   -----------------------
	 *   padded password | 32
	 *   O entry         | 32
	 *   P entry         |  4
	 *   fileID          | <fileIDLEn>
	 *  [extra padding]  | [4] (Special for step 6)
	 **/
	unsigned int size = (revision > 3 && !encMetaData) ? 72 : 68;
	encKeyWorkSpace = malloc(size + fileIDLen);

	/** Just to be sure we have no uninitalized stuff in the workspace */
	memcpy(encKeyWorkSpace, pad, 32);

	/** 3 */
	memcpy(encKeyWorkSpace + 32, ownerkey, 32);

	/** 4 */
	encKeyWorkSpace[64] = permissions & 0xff;
	encKeyWorkSpace[65] = (permissions >> 8) & 0xff;
	encKeyWorkSpace[66] = (permissions >> 16) & 0xff;
	encKeyWorkSpace[67] = (permissions >> 24) & 0xff;

	/** 5 */
	memcpy(encKeyWorkSpace + 68, fileID, fileIDLen);

	/** 6 */
	if(revision > 3 && !encMetaData) {
		encKeyWorkSpace[68+fileIDLen] = 0xff;
		encKeyWorkSpace[69+fileIDLen] = 0xff;
		encKeyWorkSpace[70+fileIDLen] = 0xff;
		encKeyWorkSpace[71+fileIDLen] = 0xff;
	}

	return size+fileIDLen;
}

#if 0
/** For debug */
static void
printHexString(const uint8_t *str, const unsigned int len) {
	unsigned int i;
	for(i=0;i<len;i++)
		printf("%x ",str[i]);
	printf("\n");
}

static void
printString(const uint8_t *str, const unsigned int len) {
	unsigned int i;
	for(i=0;i<len;i++)
		printf("%d ",str[i]);
	printf("\n");
}
#endif

/** toupper(3) but expanded to handle iso-latin-1 characters */
static uint8_t
isolat1ToUpper(const uint8_t b) {
	if(unlikely(b >= 0xe0 && b <= 0xf6))
		return b-0x20;
	else
		return toupper(b);
}

/** Really stupid permutate that needs to be replaced with a better framwork
    for smart permutations of the current password */
static bool
do_permutate(uint8_t localCurrPW[32]) {
	//static bool ret = false;
	uint8_t tmp;

	tmp = isolat1ToUpper(localCurrPW[0]);
	if(tmp != localCurrPW[0]) {
		localCurrPW[0] = tmp;
		//ret = !ret;
		return true;
	}
	else{
		return false;
		//ret = false;
	}

	//return ret;
}

/** Dummy-function to use when no permutations are wanted */
static bool
no_permutate(uint8_t localCurrPW[32]) { return false; }

/** Placeholder for the correct permutation-function to run */
static bool (*permutate)(uint8_t localCurrPW[32]) = NULL;

/** Prints out the password found */
static void
foundPassword(void) {
	char str[33];
	int fin_search;
	size_t pad_start;

	memcpy(str,currPW,currPWLen);
	str[currPWLen] = '\0';
	printf("found %s-password: '%s'\n", workWithUser?"user":"owner", str);

	/**
	 * Print out the user-password too if we know the ownerpassword.
	 * It is placed in password_user and we need to find where the pad
	 * starts before we can print it out (without ugly artifacts)
	 **/

	if(!workWithUser) {
		fin_search=-1;
		pad_start=0;

		do {
			fin_search = memcmp(password_user+pad_start, pad, 32-pad_start);
			pad_start++;
		} while (pad_start < 32 && fin_search != 0);

		memcpy(str, password_user, pad_start);
		if(!fin_search)
			str[pad_start-1] = '\0';
		printf("found user-password: '%s'\n", str);
	}
}

/** Common handling of the key for all rev3-functions */
#define RC4_DECRYPT_REV3(n) {			\
		for(i = 19; i >= 0; --i) {			\
			for(j = 0; j < length; ++j)		\
			tmpkey[j] = enckey[j] ^ i;		\
			rc4Decrypt(tmpkey, test, n, test);	\
		}						\
}

/** Checks if the rev2-password set up in encKeyWorkSpace is the correct one
    and return true if it is and false otherwise.
 */
static bool
isUserPasswordRev2(void) {
	uint8_t enckey[16];

	md5(encKeyWorkSpace, ekwlen, enckey);

	return rc4Match40b(enckey, encdata->u_string, pad);
}

/** Checks if the rev3-password set up in encKeyWorkSpace is the correct one
    and return true if it is and false otherwise.
 */
static bool
isUserPasswordRev3(void) {
	uint8_t test[16], enckey[16], tmpkey[16];
	int i;
	unsigned int length, j;

	length = encdata->length/8;
	md5(encKeyWorkSpace, ekwlen, enckey);
	md5_50(enckey);
	memcpy(test, encdata->u_string, 16);

	RC4_DECRYPT_REV3(PARTIAL_TEST_SIZE);

	/** if partial test succeeds we make a full check to be sure */
	if(unlikely(memcmp(test, rev3TestKey, PARTIAL_TEST_SIZE) == 0)) {
		memcpy(test, encdata->u_string, 16);
		RC4_DECRYPT_REV3(16);
		if(memcmp(test, rev3TestKey, 16) == 0) {
			return true;
		}
	}
	return false;
}

/** Common beginning of the main-loop in all the cracking-functions */
#define BEGIN_CRACK_LOOP() {				\
		currPWLen = setPassword(currPW);			\
		if(unlikely(lpasslength != currPWLen)) {		\
			if(likely(currPWLen < 32))			\
			memcpy(currPW + currPWLen, pad, 32-currPWLen);	\
			lpasslength = currPWLen;				\
		}							\
}

bool
runCrackRev2_o(void) {
	uint8_t enckey[16];
	unsigned int lpasslength;

	lpasslength = 0;
	startTime = time(NULL);
	do {
		BEGIN_CRACK_LOOP();

		do {
			md5(currPW, 32, enckey);

			rc4Decrypt(enckey, encdata->o_string, 32, encKeyWorkSpace);
			md5(encKeyWorkSpace, ekwlen, enckey);
			if(rc4Match40b(enckey, encdata->u_string, pad)) {
				memcpy(password_user, encKeyWorkSpace, 32);
				return true;
			}

			++nrprocessed;
		} while(permutate(currPW));
	} while(nextPassword());
	return false;
}

bool
runCrackRev3_o(void) {
	uint8_t test[32], enckey[16], tmpkey[16];
	unsigned int j, length, lpasslength;
	int i;

	length = encdata->length/8;
	lpasslength = 0;
	startTime = time(NULL);
	do {
		BEGIN_CRACK_LOOP();

		do {
			md5(currPW, 32, enckey);

			md5_50(enckey);

			memcpy(test, encdata->o_string, 32);
			RC4_DECRYPT_REV3(32);
			memcpy(encKeyWorkSpace, test, 32);

			if(isUserPasswordRev3()) {
				memcpy(password_user, encKeyWorkSpace, 32);
				return true;
			}

			++nrprocessed;
		} while(permutate(currPW));
	} while(nextPassword());
	return false;
}

bool
runCrackRev2_of(void) {
	uint8_t enckey[16];
	unsigned int lpasslength;

	lpasslength = 0;
	startTime = time(NULL);
	do {
		BEGIN_CRACK_LOOP();

		do {
			md5(encKeyWorkSpace, 32, enckey);

			/* Algorithm 3.4 reversed */
			if(rc4Match40b(enckey, encdata->o_string, password_user))
				return true;

			++nrprocessed;
		} while(permutate(currPW));
	} while(nextPassword());
	return false;
}

bool
runCrackRev3_of(void) {
	uint8_t test[32], enckey[16], tmpkey[16];
	unsigned int j, length, lpasslength;
	int i;

	length = encdata->length/8;
	lpasslength = 0;
	startTime = time(NULL);
	do {
		BEGIN_CRACK_LOOP();

		do {
			md5(encKeyWorkSpace, 32, enckey);

			md5_50(enckey);

			memcpy(test, encdata->o_string, 32);
			RC4_DECRYPT_REV3(PARTIAL_TEST_SIZE);

			/** if partial test succeeds we make a full check to be sure */
			if(unlikely(memcmp(test, password_user, PARTIAL_TEST_SIZE) == 0)) {
				memcpy(test, encdata->o_string, 32);
				RC4_DECRYPT_REV3(32);
				if(memcmp(test, password_user, 32) == 0)
					return true;
			}

			++nrprocessed;
		} while(permutate(currPW));
	} while(nextPassword());
	return false;
}

bool
runCrackRev3(void) {
	uint8_t test[16], enckey[16], tmpkey[16];
	unsigned int j, length, lpasslength;
	int i;

	length = encdata->length/8;
	lpasslength = 0;
	startTime = time(NULL);
	do {
		BEGIN_CRACK_LOOP();

		do {
			md5(encKeyWorkSpace, ekwlen, enckey);

			md5_50(enckey);
			memcpy(test, encdata->u_string, 16);

			/** Algorithm 3.5 reversed */
			RC4_DECRYPT_REV3(PARTIAL_TEST_SIZE);

			/** if partial test succeeds we make a full check to be sure */
			if(unlikely(memcmp(test, rev3TestKey, PARTIAL_TEST_SIZE) == 0)) {
				memcpy(test, encdata->u_string, 16);
				RC4_DECRYPT_REV3(16);
				if(memcmp(test, rev3TestKey, 16) == 0)
					return true;
			}

			++nrprocessed;
		} while(permutate(currPW));
	} while(nextPassword());
	return false;
}


bool
runCrackRev2(void) {
	uint8_t enckey[16];
	unsigned int lpasslength;

	lpasslength = 0;
	startTime = time(NULL);
	do {
		BEGIN_CRACK_LOOP();

		do {
			md5(encKeyWorkSpace, ekwlen, enckey);

			/* Algorithm 3.4 reversed */
			if(rc4Match40b(enckey, encdata->u_string, pad))
				return true;

			++nrprocessed;
		} while(permutate(currPW));
	} while(nextPassword());
	return false;
}


void
crackRev2Thread(void* vThreadId) {
	//instance of a thread

	int threadId = *((int*)vThreadId);
	printf("thread %i started\n", threadId);
	uint8_t enckey[16];

	// thread's local copy of global variables
	uint8_t localEncKeyWorkSpace[ekwlen];
	uint8_t localCurrPW[33];
	unsigned int localCurrPWLen=0;
	uint8_t localUString[32];
	memcpy(localUString, encdata->u_string, 32);

	unsigned int lpasslength;
	int ack;

	lpasslength = 0;
	startTime = time(NULL);


	// Access to the parameter endOfSearch, currPW, currPWLen and encKeyWorkSpace has to be protected
	if (pthread_mutex_lock(&lock) != 0) printf("Mutex 1: lock failed\n");
	if (!endOfSearch) {
		memcpy(localEncKeyWorkSpace, encKeyWorkSpace, ekwlen);
		memcpy(localCurrPW, currPW, 32);
		localCurrPWLen = currPWLen;
		localCurrPW[localCurrPWLen] = 0;
		BEGIN_CRACK_LOOP();
	}
	if (pthread_mutex_unlock(&lock) != 0) printf("Mutex 1: unlock failed\n");


	while(!endOfSearch) { //can be set by another thread as well


		do { // permutation

			//printf("Thread %i: curPassword:  '%s'\n", threadId, localCurrPW);


			md5(localEncKeyWorkSpace, ekwlen, enckey);

			/* Algorithm 3.4 reversed */
			if(rc4Match40b(enckey, localUString, pad)){
					// Access to the parameter endOfSearch, currPW, currPWLen and encKeyWorkSpace has to be protected
					if (pthread_mutex_lock(&lock) != 0) printf("Mutex 2: lock failed\n");
					printf("Thread %i found password: '%s'\n", threadId, localCurrPW);
					endOfSearch = true;
					pwFound = true;
					// Key found. Copy results to global parameters (from the thread's local copy)
					memcpy(currPW, localCurrPW, 32);
					currPWLen = localCurrPWLen;
					if (pthread_mutex_unlock(&lock) != 0) printf("Mutex 2: unlock failed\n");

					return; //finish thread
			}

			++nrprocessed;
		} while(permutate(localCurrPW));

		// Access to the parameter endOfSearch, currPW, currPWLen and encKeyWorkSpace has to be protected
		ack = pthread_mutex_lock(&lock);
		if (ack != 0) printf("Mutex 3: lock failed. code: %i\n",ack);
		if (!endOfSearch) {
			endOfSearch = !nextPassword();
			generatedPWCounts[threadId] = globalGenPwdCnt++;
			// make local copy of currPW, currPWLen and encKeyWorkSpace
			memcpy(localEncKeyWorkSpace, encKeyWorkSpace, ekwlen);
			memcpy(localCurrPW, currPW, 32);
			localCurrPWLen = currPWLen;
			localCurrPW[localCurrPWLen] = 0;
			//printf("thread %i: nextPassword: %s\n", threadId, localCurrPW);

			//memcpy(nonPermutatedPwds[threadId], localCurrPW, 33);
			BEGIN_CRACK_LOOP();

		}
		ack = pthread_mutex_unlock(&lock);
		if (ack != 0) printf("Mutex 3: unlock failed. code: %i\n",ack);

	}

	return;
}



void
crackRev3Thread(void* vThreadId) {
	//instance of a thread

	int threadId = *((int*)vThreadId);
	printf("thread %i started\n", threadId);
	uint8_t test[16], enckey[16], tmpkey[16];

	// thread's local copy of global variables
	uint8_t localEncKeyWorkSpace[ekwlen];
	uint8_t localCurrPW[33];
	unsigned int localCurrPWLen=0;

	unsigned int j, length, lpasslength;
	int i;
	int ack;

	length = encdata->length/8;
	lpasslength = -1;
	startTime = time(NULL);

	// Access to the parameter endOfSearch, currPW, currPWLen and encKeyWorkSpace has to be protected
	if (pthread_mutex_lock(&lock) != 0) printf("Mutex 1: lock failed\n");
	if (!endOfSearch) {
		memcpy(localEncKeyWorkSpace, encKeyWorkSpace, ekwlen);
		memcpy(localCurrPW, currPW, 32);
		localCurrPWLen = currPWLen;
		localCurrPW[localCurrPWLen] = 0;
		BEGIN_CRACK_LOOP();
	}
	if (pthread_mutex_unlock(&lock) != 0) printf("Mutex 1: unlock failed\n");


	while(!endOfSearch) { //can be set by another thread as well


		do { // permutation

			//printf("Thread %i: curPassword:  '%s'\n", threadId, localCurrPW);

			md5(localEncKeyWorkSpace, ekwlen, enckey);
			md5_50(enckey);
			memcpy(test, encdata->u_string, 16);

			/** Algorithm 3.5 reversed */
			RC4_DECRYPT_REV3(PARTIAL_TEST_SIZE);

			/** if partial test succeeds we make a full check to be sure */
			if(unlikely(memcmp(test, rev3TestKey, PARTIAL_TEST_SIZE) == 0)) {
				memcpy(test, encdata->u_string, 16);
				RC4_DECRYPT_REV3(16);
				if(memcmp(test, rev3TestKey, 16) == 0) {
					// Access to the parameter endOfSearch, currPW, currPWLen and encKeyWorkSpace has to be protected
					if (pthread_mutex_lock(&lock) != 0) printf("Mutex 2: lock failed\n");
					printf("Thread %i found password: '%s'\n", threadId, localCurrPW);
					endOfSearch = true;
					pwFound = true;
					// Key found. Copy results to global parameters (from the thread's local copy)
					memcpy(currPW, localCurrPW, 32);
					currPWLen = localCurrPWLen;
					if (pthread_mutex_unlock(&lock) != 0) printf("Mutex 2: unlock failed\n");

					return; //finish thread
				}
			}

			++nrprocessed;
		} while(permutate(localCurrPW));

		// Access to the parameter endOfSearch, currPW, currPWLen and encKeyWorkSpace has to be protected
		ack = pthread_mutex_lock(&lock);
		if (ack != 0) printf("Mutex 3: lock failed. code: %i\n",ack);
		if (!endOfSearch) {
			endOfSearch = !nextPassword();
//			printf("thread %i: ", threadId);
//			printCurrPw();
			generatedPWCounts[threadId] = globalGenPwdCnt++;
			BEGIN_CRACK_LOOP();
			// make local copy of currPW, currPWLen and encKeyWorkSpace
			memcpy(localEncKeyWorkSpace, encKeyWorkSpace, ekwlen);
			memcpy(localCurrPW, currPW, 32);
			localCurrPWLen = currPWLen;
			localCurrPW[localCurrPWLen] = 0;
			//printf("thread %i: nextPassword: %s\n", threadId, localCurrPW);

			memcpy(nonPermutatedPwds[threadId], localCurrPW, 33);

		}
		ack = pthread_mutex_unlock(&lock);
		if (ack != 0) printf("Mutex 3: unlock failed. code: %i\n",ack);
		//if ((ack = pthread_mutex_unlock(&lock)) != 0) printf("Mutex 3: unlock failed! code: %i\n");
	}

	return;
}

//bool
//runCrackRev3MultiThreads(void) {
//	pthread_t pThread[nrOfThreads];
//	int i;
//	int threadId[nrOfThreads];
//
//	generatedPWCounts = (long*)malloc(nrOfThreads * sizeof(long));
//	nonPermutatedPwds = (unsigned char**)malloc(nrOfThreads * sizeof(unsigned char*));
//	for (i = 0; i < nrOfThreads; i++) {
//		nonPermutatedPwds[i] = (unsigned char*)malloc(33 * sizeof(unsigned char));
//	}
//
//	pwFound = false;
//	endOfSearch = false;
//
//	// start multiple threads
//	for (i = 0; i < nrOfThreads; i++) {
//		threadId[i] = i;
//		pthread_create(&pThread[i],NULL,&crackRev3Thread,(void*)&threadId[i]);
//	}
//
//	// wait for threads to be finished
//	for (i = 0; i < nrOfThreads; i++) {
//		pthread_join(pThread[i],NULL);
//		printf("joined thread %i\n", i);
//	}
//
//	return pwFound;
//}

bool
runCrackMultiThreads() {
	pthread_t pThread[nrOfThreads];
	int i;
	int threadId[nrOfThreads];

	if (!workWithUser) {
		printf("Multithread operation only supports user-password search.\n");
		return false;
	}


	generatedPWCounts = (long*)malloc(nrOfThreads * sizeof(long));
	nonPermutatedPwds = (unsigned char**)malloc(nrOfThreads * sizeof(unsigned char*));
	for (i = 0; i < nrOfThreads; i++) {
		nonPermutatedPwds[i] = (unsigned char*)malloc(33 * sizeof(unsigned char));
	}
	globalGenPwdCnt = 0;

	pwFound = false;
	endOfSearch = false;


	// init mutex
	pthread_mutex_init(&lock, NULL);

	// start multiple threads
	for (i = 0; i < nrOfThreads; i++) {
		threadId[i] = i;
		if (encdata->revision == 2) { // 40 bit rc4
			pthread_create(&pThread[i],NULL,&crackRev2Thread,(void*)&threadId[i]);
		}
		else { // rev3+ (40-128bit aes or rc4)
			pthread_create(&pThread[i],NULL,&crackRev3Thread,(void*)&threadId[i]);
		}
	}

	// wait for threads to be finished
	for (i = 0; i < nrOfThreads; i++) {
		pthread_join(pThread[i],NULL);
		printf("joined thread %i\n", i);
	}

	crackDone = true;


	return pwFound;
}


/** Start cracking and does not stop until it has either been interrupted by
    a signal or the password either is found or wordlist or charset is exhausted
 */
void
runCrack(void) {
	bool found = false;
	uint8_t cpw[32];
	if (nrOfThreads > 0){
		found = runCrackMultiThreads();
	}
	else {

		if(!workWithUser && !knownPassword) {
			memcpy(cpw, pad, 32);
			currPW = cpw;
			if(encdata->revision == 2)
				found = runCrackRev2_o();
			else
				found = runCrackRev3_o();
		}
		else if(encdata->revision == 2) {
			if(workWithUser)
				found = runCrackRev2();
			else /** knownPassword */
				found = runCrackRev2_of();
		}
		else {
			if(workWithUser) {
				found = runCrackRev3();
			} else { /** knownPassword */
				found = runCrackRev3_of();
			}
		}
		crackDone = true;
	}

	if(!found){
		printf("Could not find password\n");
	}
	else{
		foundPassword();
	}
	currPW = NULL;
}

/** returns the number of processed passwords */
unsigned int
getNrProcessed(void) { return nrprocessed; }

/** These are shared variables between loading and initialisation and controls
    how to do the initialisation. Should not be touched by anything except
    loadState and cleanPDFCrack.
 */
static bool recovery = false;
static bool permutation = false;


/** cleans up everything as is needed to do a any initPDFCrack-calls after the
    first one.
 */
void
cleanPDFCrack(void) {
	if(rev3TestKey) {
		/** Do a really ugly const to non-const cast but this one time it should
	be safe
		 */
		free((uint8_t*)rev3TestKey);
		rev3TestKey = NULL;
	}
	if(encKeyWorkSpace) {
		free(encKeyWorkSpace);
		encKeyWorkSpace = NULL;
	}
	knownPassword = false;
	recovery = false;
	permutation = false;
}

/** initPDFCrack is doing all the initialisations before you are able to call
    runCrack(). Make sure that you run cleanPDFCrack before you call this
    after the first time.
 */
bool
initPDFCrack(const EncData *e, const uint8_t *upw, const bool user,
		const char *wl, const passwordMethod pm, FILE *file,
		const char *cs, const unsigned int minPw,
		const unsigned int maxPw, const bool perm, const int nrOfThreadsLoc,
		const int zone_local, const int nrOfZones_local) {
	uint8_t buf[128];
	unsigned int upwlen;
	uint8_t *tmp;

	ekwlen = initEncKeyWorkSpace(e->revision, e->encryptMetaData, e->permissions,
			e->o_string, e->fileID, e->fileIDLen);

	encdata = e;
	currPW = encKeyWorkSpace;
	currPWLen = 0;
	nrprocessed = 0;
	workWithUser = user;
	crackDone = false;
	setrc4DecryptMethod((const unsigned int)e->length);
	if(upw) {
		upwlen = strlen((const char*)upw);
		if(upwlen > 32)
			upwlen = 32;
		memcpy(password_user, upw, upwlen);
		memcpy(password_user+upwlen, pad, 32-upwlen);
		memcpy(encKeyWorkSpace, password_user, 32);
		knownPassword = true;
	}
	/** Workaround to set password_user when loading state from file */
	if(recovery)
		memcpy(encKeyWorkSpace, password_user, 32);

	if(encdata->revision == 2) {
		if(knownPassword) {
			if(!isUserPasswordRev2())
				return false;
			memcpy(encKeyWorkSpace, pad, 32);
		}
		else {
			memcpy(password_user, pad, 32);
			knownPassword = isUserPasswordRev2();
		}
	}
	else if(e->revision >= 3) {
		memcpy(buf, pad, 32);
		memcpy(buf + 32, e->fileID, e->fileIDLen);
		tmp = malloc(sizeof(uint8_t)*16);
		md5(buf, 32+e->fileIDLen, tmp);
		rev3TestKey = tmp;
		if(knownPassword) {
			if(!isUserPasswordRev3())
				return false;
			memcpy(encKeyWorkSpace, pad, 32);
		}
		else {
			memcpy(password_user, pad, 32);
			knownPassword = isUserPasswordRev3();
		}
	}

	permutation = (perm || permutation);
	if(permutation)
		permutate = do_permutate;
	else
		permutate = no_permutate;

	nrOfThreads = nrOfThreadsLoc;

	if (nrOfZones_local > 1) {
		printf("Working on zone %i of %i.\n", zone_local, nrOfZones_local);
	}

	initPasswords(pm, file, wl, cs, minPw, maxPw, zone_local, nrOfZones_local);
	return true;
}

/** Some common patterns between the loadState and saveState */
static const char string_PRVPL[] =
		"PDF: %d.%d\nR: %d\nV: %d\nP: %d\nL: %d\n"
		"MetaData: %d\nFileID(%d):";
static const char string_FILTER[] = "\nFilter(%zu): ";
static const char string_UUPWP[] =
		"\nUser: %d\nUserPw: %d\nPermutate: %d\n";


/** Reads from file and tries to set up the state that it contains.
    Returns true on success and false otherwise.
    Very little checks for data validitiy is made.
 */
bool
loadState(FILE *file, EncData *e, char **wl, bool *user) {
	unsigned int i;
	int tmp, tmp2, tmp3, len;
	char c;

	/** Load all the simple values bound to the document */
	if(fscanf(file,string_PRVPL, &e->version_major, &e->version_minor,
			&e->revision, &e->version, &e->permissions, &e->length,
			&tmp, &e->fileIDLen) < 8)
		return false;

	/** bork out if length is insanely high */
	if(e->fileIDLen > 256)
		return false;

	e->encryptMetaData = (tmp == true);

	/** Load the FileID */
	e->fileID = malloc(sizeof(uint8_t)*e->fileIDLen);
	for(i=0;i<e->fileIDLen;i++) {
		if(fscanf(file, " %d", &tmp) < 1)
			return false;
		e->fileID[i] = tmp;
	}

	/** Load the Security Handler */
	if(fscanf(file,string_FILTER, &len) < 1)
		return false;

	/** bork out if length is insanely high or negative */
	if(len > 256 || len < 0)
		return false;

	if(len > 0)
		e->s_handler = malloc((sizeof(uint8_t)*len)+1);

	for(i=0;i<(unsigned int)len;i++) {
		if(fscanf(file, "%c", &c) < 1)
			return false;
		e->s_handler[i] = c;
	}
	/** Make sure we null-terminate the string */
	e->s_handler[i] = '\0';

	/** Load the U- and O-strings */
	if(fscanf(file, "\nO:") == EOF)
		return false;
	e->o_string = malloc(sizeof(uint8_t)*32);
	e->u_string = malloc(sizeof(uint8_t)*32);
	for(i=0;i<32;i++) {
		if(fscanf(file, " %d", &tmp) < 1)
			return false;
		e->o_string[i] = tmp;
	}
	if(fscanf(file, "\nU:") == EOF)
		return false;
	for(i=0;i<32;i++) {
		if(fscanf(file, " %d", &tmp) < 1)
			return false;
		e->u_string[i] = tmp;
	}

	/** Load the simple values bound to the state */
	if(fscanf(file, string_UUPWP, &tmp, &tmp2, &tmp3) < 3)
		return false;
	*user = (tmp == true);
	knownPassword = (tmp2 == true);
	permutation = (tmp3 == true);

	/** Load the saved userpassword if that is used */
	if(knownPassword) {
		for(i=0;i<32;i++) {
			if(fscanf(file, " %d", &tmp) < 1)
				return false;
			password_user[i] = tmp;
		}
	}

	/** Load the password-specific stuff for the state */
	if(!pw_loadState(file, wl))
		return false;

	/** Remember to set the recovery-boolean to make sure the right things
      happen in initPDFCrack */
	recovery = true;

	return true;
}

/** Saves the current state in the engine to file */
void
saveState(FILE *file) {
	unsigned int i;

	fprintf(file, string_PRVPL,
			encdata->version_major, encdata->version_minor, encdata->revision,
			encdata->version, encdata->permissions, encdata->length,
			(int)encdata->encryptMetaData, encdata->fileIDLen);
	for(i=0;i<encdata->fileIDLen;i++)
		fprintf(file, " %d", encdata->fileID[i]);
	fprintf(file, string_FILTER, strlen(encdata->s_handler));
	fprintf(file, "%s", encdata->s_handler);
	fprintf(file, "\nO:");
	for(i=0;i<32;i++)
		fprintf(file, " %d", encdata->o_string[i]);
	fprintf(file,"\nU:");
	for(i=0;i<32;i++)
		fprintf(file, " %d", encdata->u_string[i]);
	fprintf(file, string_UUPWP, (int)workWithUser,
			(int)knownPassword, (int)permutation);
	if(knownPassword) {
		for(i=0;i<32;i++)
			fprintf(file, " %d", password_user[i]);
	}

	/** Save the password-specific stuff */
	pw_saveState(file);
}

int getNrOfThreads(void){
	return nrOfThreads;
}

void setPwdMultiThread(int *passwordOut, const uint8_t *charset, unsigned int charsetLen){

//	if (nrOfThreads <= 1) {
//		return;
//	}

	int success = pthread_mutex_lock(&lock);
	if (success != 0) {
		printf("Mutex lock save pw failed: error %i\n", success);
	}

	int i;
	unsigned int j;
	int currThreadId=0;
//	printf("Thread %i, generated PW Count %ld, pw; '%s'\n", 0,
//			generatedPWCounts[0], nonPermutatedPwds[0]);
	for(i = 1; i < nrOfThreads;i++){
//		printf("Thread %i, generated PW Count %ld, pw; '%s'\n", i,
//				generatedPWCounts[i], nonPermutatedPwds[i]);
		if (generatedPWCounts[i] < generatedPWCounts[currThreadId]) {
			currThreadId = i;
		}
	}

//	printf("nonPermutatedPwds[currThreadId]: %s\n", nonPermutatedPwds[currThreadId]);

	int len = strlen((char*)nonPermutatedPwds[currThreadId]);

//	i = 0;
//	while (nonPermutatedPwds[currThreadId][i] != 0 && i < 32) {
//		for(j=0; j < charsetLen; j ++){
//			if (nonPermutatedPwds[currThreadId][i] == charset[j]) {
//				password[i] = j;
//			}
//		}
//		if (password[i] == -1) {
//			printf("Failed to save password in multi-thread mode: pw'%s'\n",
//					nonPermutatedPwds[currThreadId]);
//		}
//		i++;
//	}
//
//	for (; i < 32; i++) {
//		password[i] = -1;
//
//	}

	for (i = len; i < 32; i++){
		passwordOut[i] = -1;
	}

	for (i = 0; i < len; i++){
		for(j=0; j < charsetLen; j ++){
			if (nonPermutatedPwds[currThreadId][i] == charset[j]) {
				passwordOut[i] = j;
			}
		}
		if (passwordOut[i] == -1) {
			printf("Failed to save password in multi-thread mode: pw '%s'\n", nonPermutatedPwds[currThreadId]);
		}
	}

//	printf("saving pwd '%s'\n", nonPermutatedPwds[currThreadId]);



	success = pthread_mutex_unlock(&lock);
	if (success != 0) {
		printf("Mutex unlock save pw failed: error %i\n", success);
	}




}
