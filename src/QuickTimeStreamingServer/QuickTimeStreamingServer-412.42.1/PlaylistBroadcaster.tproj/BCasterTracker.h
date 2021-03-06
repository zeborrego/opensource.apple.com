
/*
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * Copyright (c) 1999-2001 Apple Computer, Inc.  All Rights Reserved. The
 * contents of this file constitute Original Code as defined in and are
 * subject to the Apple Public Source License Version 1.2 (the 'License').
 * You may not use this file except in compliance with the License.  Please
 * obtain a copy of the License at http://www.apple.com/publicsource and
 * read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.  Please
 * see the License for the specific language governing rights and
 * limitations under the License.
 *
 *
 * @APPLE_LICENSE_HEADER_END@
 *
 */

#ifndef __BCASTERTRACKER__
#define __BCASTERTRACKER__

#include <stdio.h>
#include "TrackingElement.h"
#include "PLDoubleLinkedList.h"

class BCasterTracker 
{
	public:
								BCasterTracker( const char* fName );
		virtual 				~BCasterTracker();
	
		int						RemoveByProcessID( pid_t pid );
		int						Remove( UInt32 itemID );
		int						Add( pid_t pid, const char* bcastList );
		int						Save();
		void					Show();
		static bool			IsProcessRunning( pid_t pid );
		bool					IsOpen();
		
	protected:

		enum {  kTrackerLineBuffSize = 512 };
		
		FILE*					mTrackerFile;
		long					mEofPos;
		
		PLDoubleLinkedList<TrackingElement>	mTrackingList;
		
};

void TestBCasterTracker(int x);

#endif
