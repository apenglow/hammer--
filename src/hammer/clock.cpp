//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#ifdef _WIN32
#include <stdafx.h>
#else
#include "linux_pch.h"
#endif

static double beginTime;


double I_FloatTime( void )
{
#ifdef _WIN32
	static double freq = 0.0;
	static __int64 firstCount;
	__int64	curCount;

	if (freq == 0.0)
	{
		__int64 perfFreq;
		QueryPerformanceFrequency( (LARGE_INTEGER*)&perfFreq );
		QueryPerformanceCounter( (LARGE_INTEGER*)&firstCount );
		freq = 1.0 / (double)perfFreq;
	}

	QueryPerformanceCounter ( (LARGE_INTEGER*)&curCount );
	curCount -= firstCount;
	double time = (double)curCount * freq;
	return time;
#else
	using Clock = std::chrono::steady_clock;
	static const Clock::time_point first = Clock::now();
	return std::chrono::duration<double>( Clock::now() - first ).count();
#endif
}


void I_BeginTime( void )
{
	beginTime = I_FloatTime();
}


double I_EndTime( void )
{
	return ( I_FloatTime() - beginTime );
}
