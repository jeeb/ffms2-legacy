//  Copyright (c) 2007-2009 Fredrik Mellbin
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include "ffaudiosource.h"

/* Audio Cache */

TAudioBlock::TAudioBlock(int64_t Start, int64_t Samples, uint8_t *SrcData, size_t SrcBytes) {
	this->Start = Start;
	this->Samples = Samples;
	Data = new uint8_t[SrcBytes];
	memcpy(Data, SrcData, SrcBytes);
}

TAudioBlock::~TAudioBlock() {
	delete[] Data;
}

TAudioCache::TAudioCache() {
	MaxCacheBlocks = 0;
	BytesPerSample = 0;
}

TAudioCache::~TAudioCache() {
	for (TAudioCache::iterator it=begin(); it != end(); it++)
		delete *it;
}

void TAudioCache::Initialize(int BytesPerSample, int MaxCacheBlocks) {
	this->BytesPerSample = BytesPerSample;
	this->MaxCacheBlocks = MaxCacheBlocks;
}

void TAudioCache::CacheBlock(int64_t Start, int64_t Samples, uint8_t *SrcData) {
	if (BytesPerSample > 0) {
		for (TAudioCache::iterator it=begin(); it != end(); it++) {
			if ((*it)->Start == Start) {
				delete *it;
				erase(it);
				break;
			}
		}

		push_front(new TAudioBlock(Start, Samples, SrcData, static_cast<size_t>(Samples * BytesPerSample)));
		if (static_cast<int>(size()) >= MaxCacheBlocks) {
			delete back();
			pop_back();
		}
	}
}

bool TAudioCache::AudioBlockComp(TAudioBlock *A, TAudioBlock *B) {
	return A->Start < B->Start;
}

int64_t TAudioCache::FillRequest(int64_t Start, int64_t Samples, uint8_t *Dst) {
	// May be better to move used blocks to the front
	std::list<TAudioBlock *> UsedBlocks;
	for (TAudioCache::iterator it=begin(); it != end(); it++) {
		int64_t SrcOffset = FFMAX(0, Start - (*it)->Start);
		int64_t DstOffset = FFMAX(0, (*it)->Start - Start);
		int64_t CopySamples = FFMIN((*it)->Samples - SrcOffset, Samples - DstOffset);
		if (CopySamples > 0) {
			memcpy(Dst + DstOffset * BytesPerSample, (*it)->Data + SrcOffset * BytesPerSample, static_cast<size_t>(CopySamples * BytesPerSample));
			UsedBlocks.push_back(*it);
		}
	}
	UsedBlocks.sort(AudioBlockComp);
	int64_t Ret = Start;
	for (std::list<TAudioBlock *>::iterator it = UsedBlocks.begin(); it != UsedBlocks.end(); it++) {
		if (it == UsedBlocks.begin() || Ret == (*it)->Start)
			Ret = (*it)->Start + (*it)->Samples;
		else
			break;
	}
	return FFMIN(Ret, Start + Samples);
}

/* FFAudio base class */

FFAudio::FFAudio(const char *SourceFile, FFIndex *Index, char *ErrorMsg, unsigned MsgSize) {
	if (Index->CompareFileSignature(SourceFile, ErrorMsg, MsgSize))
		throw ErrorMsg;

	CurrentSample = 0;
	DecodingBuffer = new uint8_t[AVCODEC_MAX_AUDIO_FRAME_SIZE * 10];
}

FFAudio::~FFAudio() {
	delete[] DecodingBuffer;
}
