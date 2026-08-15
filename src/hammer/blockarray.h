//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef _BLOCKARRAY_H
#define _BLOCKARRAY_H

#ifdef _WIN32
#include "tier0/dbg.h"
// memdbgon must be the last include file in a .cpp file!!!
#include <tier0/memdbgon.h>
#else
#include <stdexcept>
#include <string>
#endif

template <class T, int nBlockSize, int nMaxBlocks>
class BlockArray
{
public:
	BlockArray()
	{
		nCount = nBlocks = 0;
	}
	~BlockArray()
	{
		GetBlocks(0);
	}

	T& operator[] (int iIndex);
	
	void SetCount(int nObjects);
	int GetCount() { return nCount; }

private:
	T * Blocks[nMaxBlocks+1];
	short nCount;
	short nBlocks;
	void GetBlocks(int nNewBlocks);
};

template <class T, int nBlockSize, int nMaxBlocks>
void BlockArray<T,nBlockSize,nMaxBlocks>::
	GetBlocks(int nNewBlocks)
{
	for(int i = nBlocks; i < nNewBlocks; i++)
	{
		Blocks[i] = new T[nBlockSize];
	}
	for(int i = nNewBlocks; i < nBlocks; i++)
	{
		delete[] Blocks[i];
	}

	nBlocks = nNewBlocks;
}

template <class T, int nBlockSize, int nMaxBlocks>
void BlockArray<T,nBlockSize,nMaxBlocks>::
	SetCount(int nObjects)
{
	if(nObjects == nCount)
		return;

	// find the number of blocks required by nObjects, checking for
	// integer rounding error
	int nNewBlocks = (nObjects / nBlockSize);
	if ((nNewBlocks * nBlockSize) < nObjects)
	{
		nNewBlocks++;
	}

	if(nNewBlocks != nBlocks)
	{
		// Make sure we don't get an overrun.
		if ( nNewBlocks > nMaxBlocks + 1 )
		{
#ifdef _WIN32
			Error( "BlockArray< ?, %d, %d > - too many blocks needed.", nBlockSize, nMaxBlocks );
#else
			throw std::length_error( "BlockArray capacity exceeded" );
#endif
		}
		
		GetBlocks(nNewBlocks);
	}
	nCount = nObjects;
}

template <class T, int nBlockSize, int nMaxBlocks>
T& BlockArray<T,nBlockSize,nMaxBlocks>::operator[] (int iIndex)
{
	if(iIndex < 0)
	{
#ifdef _WIN32
		Error( "BlockArray< %d, %d > - invalid block index.", iIndex, nCount );
#else
		throw std::out_of_range( "negative BlockArray index" );
#endif
	}
	if(iIndex >= nCount)
	{
		SetCount(iIndex+1);
	}
	return Blocks[iIndex / nBlockSize][iIndex % nBlockSize];
}

#ifdef _WIN32
#include <tier0/memdbgoff.h>
#endif

#endif // _BLOCKARRAY_H