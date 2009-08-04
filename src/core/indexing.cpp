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

#include "indexing.h"
#include <iostream>
#include <fstream>
#include <algorithm>

extern "C" {
#include <libavutil/sha1.h>
}



extern bool HasHaaliMPEG;
extern bool HasHaaliOGG;

struct IndexHeader {
	uint32_t Id;
	uint32_t Version;
	uint32_t Tracks;
	uint32_t Decoder;
	uint32_t LAVUVersion;
	uint32_t LAVFVersion;
	uint32_t LAVCVersion;
	uint32_t LSWSVersion;
	uint32_t LPPVersion;
	int64_t FileSize;
	uint8_t FileSignature[20];
};

SharedVideoContext::SharedVideoContext(bool FreeCodecContext) {
	CodecContext = NULL;
	Parser = NULL;
	CS = NULL;
	this->FreeCodecContext = FreeCodecContext;
}

SharedVideoContext::~SharedVideoContext() {
	if (CodecContext) {
		avcodec_close(CodecContext);
		if (FreeCodecContext)
			av_freep(&CodecContext);
	}

	if (Parser)
		av_parser_close(Parser);

	if (CS)
		cs_Destroy(CS);
}

SharedAudioContext::SharedAudioContext(bool FreeCodecContext) {
	W64Writer = NULL;
	CodecContext = NULL;
	CurrentSample = 0;
	CS = NULL;
	this->FreeCodecContext = FreeCodecContext;
}

SharedAudioContext::~SharedAudioContext() {
	delete W64Writer;
	if (CodecContext) {
		avcodec_close(CodecContext);
		if (FreeCodecContext)
			av_freep(&CodecContext);
	}

	if (CS)
		cs_Destroy(CS);
}

TFrameInfo::TFrameInfo(int64_t DTS, int64_t SampleStart, int RepeatPict, bool KeyFrame, int64_t FilePos, unsigned int FrameSize) {
	this->DTS = DTS;
	this->RepeatPict = RepeatPict;
	this->KeyFrame = KeyFrame;
	this->SampleStart = SampleStart;
	this->FilePos = FilePos;
	this->FrameSize = FrameSize;
}

TFrameInfo TFrameInfo::VideoFrameInfo(int64_t DTS, int RepeatPict, bool KeyFrame, int64_t FilePos, unsigned int FrameSize) {
	return TFrameInfo(DTS, 0, RepeatPict, KeyFrame, FilePos, FrameSize);
}

TFrameInfo TFrameInfo::AudioFrameInfo(int64_t DTS, int64_t SampleStart, bool KeyFrame, int64_t FilePos, unsigned int FrameSize) {
	return TFrameInfo(DTS, SampleStart, 0, KeyFrame, FilePos, FrameSize);
}

void FFMS_Track::WriteTimecodes(const char *TimecodeFile) {
	ffms_fstream Timecodes(TimecodeFile, std::ios::out | std::ios::trunc);

	if (!Timecodes.is_open())
		throw FFMS_Exception(FFMS_ERROR_TRACK, FFMS_ERROR_FILE_WRITE,
			boost::format("Failed to open '%1%' for writing") % TimecodeFile);

	Timecodes << "# timecode format v2\n";

	for (iterator Cur=begin(); Cur!=end(); Cur++)
		Timecodes << std::fixed << ((Cur->DTS * TB.Num) / (double)TB.Den) << "\n";
}

int FFMS_Track::FrameFromDTS(int64_t DTS) {
	for (int i = 0; i < static_cast<int>(size()); i++)
		if (at(i).DTS == DTS)
			return i;
	return -1;
}

int FFMS_Track::ClosestFrameFromDTS(int64_t DTS) {
	int Frame = 0; 
	int64_t BestDiff = 0xFFFFFFFFFFFFFFLL; // big number
	for (int i = 0; i < static_cast<int>(size()); i++) {
		int64_t CurrentDiff = FFABS(at(i).DTS - DTS);
		if (CurrentDiff < BestDiff) {
			BestDiff = CurrentDiff;
			Frame = i;
		}
	}

	return Frame;
}

int FFMS_Track::FindClosestVideoKeyFrame(int Frame) {
	Frame = FFMIN(FFMAX(Frame, 0), static_cast<int>(size()) - 1);
	for (int i = Frame; i > 0; i--)
		if (at(i).KeyFrame)
			return i;
	return 0;
}

int FFMS_Track::FindClosestAudioKeyFrame(int64_t Sample) {
	for (size_t i = 0; i < size(); i++) {
		if (at(i).SampleStart == Sample && at(i).KeyFrame)
			return i;
		else if (at(i).SampleStart > Sample && at(i).KeyFrame)
			return i  - 1;
	}
	return size() - 1;
}

FFMS_Track::FFMS_Track() {
	this->TT = FFMS_TYPE_UNKNOWN;
	this->TB.Num = 0; 
	this->TB.Den = 0;
}

FFMS_Track::FFMS_Track(int64_t Num, int64_t Den, FFMS_TrackType TT) {
	this->TT = TT;
	this->TB.Num = Num; 
	this->TB.Den = Den;
}

void FFMS_Index::CalculateFileSignature(const char *Filename, int64_t *Filesize, uint8_t Digest[20]) {
	FILE *SFile = ffms_fopen(Filename,"rb");

	if (SFile == NULL)
		throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_FILE_READ,
			boost::format("Failed to open '%1%' for hashing") % Filename);

	const int BlockSize = 1024*1024;
	std::vector<uint8_t> FileBuffer(BlockSize);
	std::vector<uint8_t> ctxmem(av_sha1_size);
	AVSHA1 *ctx = (AVSHA1 *)&ctxmem[0];
	av_sha1_init(ctx);
	
	memset(&FileBuffer[0], 0, BlockSize);
	fread(&FileBuffer[0], 1, BlockSize, SFile);
	if (ferror(SFile) && !feof(SFile)) {
		av_sha1_final(ctx, Digest);
		fclose(SFile);
		throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_FILE_READ,
			boost::format("Failed to read '%1%' for hashing") % Filename);
	}
	av_sha1_update(ctx, &FileBuffer[0], BlockSize);

	fseeko(SFile, -BlockSize, SEEK_END);
	memset(&FileBuffer[0], 0, BlockSize);
	fread(&FileBuffer[0], 1, BlockSize, SFile);
	if (ferror(SFile) && !feof(SFile)) {
		av_sha1_final(ctx, Digest);
		fclose(SFile);
		throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_FILE_READ,
			boost::format("Failed to seek with offset %1% from file end in '%2%' for hashing") % BlockSize % Filename);
	}
	av_sha1_update(ctx, &FileBuffer[0], BlockSize);

	fseeko(SFile, 0, SEEK_END);
	if (ferror(SFile)) {
		av_sha1_final(ctx, Digest);
		fclose(SFile);
		throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_FILE_READ,
			boost::format("Failed to seek to end of '%1%' for hashing") % Filename);
	}
	*Filesize = ftello(SFile);
	fclose(SFile);

	av_sha1_final(ctx, Digest);
}

static bool DTSComparison(TFrameInfo FI1, TFrameInfo FI2) {
	return FI1.DTS < FI2.DTS;
}

void FFMS_Index::Sort() {
	for (FFMS_Index::iterator Cur=begin(); Cur!=end(); Cur++)
		std::sort(Cur->begin(), Cur->end(), DTSComparison);
}

bool FFMS_Index::CompareFileSignature(const char *Filename) {
	int64_t CFilesize;
	uint8_t CDigest[20];
	CalculateFileSignature(Filename, &CFilesize, CDigest);
	return (CFilesize == Filesize && !memcmp(CDigest, Digest, sizeof(Digest)));
}

void FFMS_Index::WriteIndex(const char *IndexFile) {
	ffms_fstream IndexStream(IndexFile, std::ios::out | std::ios::binary | std::ios::trunc);

	if (!IndexStream.is_open()) 
		throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_FILE_WRITE,
			boost::format("Failed to open '%1%' for writing") % IndexFile);

	// Write the index file header
	IndexHeader IH;
	IH.Id = INDEXID;
	IH.Version = INDEXVERSION;
	IH.Tracks = size();
	IH.Decoder = Decoder;
	IH.LAVUVersion = avutil_version();
	IH.LAVFVersion = avformat_version();
	IH.LAVCVersion = avcodec_version();
	IH.LSWSVersion = swscale_version();
	IH.LPPVersion = postproc_version();
	IH.FileSize = Filesize;
	memcpy(IH.FileSignature, Digest, sizeof(Digest));

	IndexStream.write(reinterpret_cast<char *>(&IH), sizeof(IH));
	
	for (unsigned int i = 0; i < IH.Tracks; i++) {
		uint32_t TT = at(i).TT;
		IndexStream.write(reinterpret_cast<char *>(&TT), sizeof(TT));
		int64_t Num = at(i).TB.Num;
		IndexStream.write(reinterpret_cast<char *>(&Num), sizeof(Num));
		int64_t Den = at(i).TB.Den;
		IndexStream.write(reinterpret_cast<char *>(&Den), sizeof(Den));
		int64_t Frames = at(i).size();
		IndexStream.write(reinterpret_cast<char *>(&Frames), sizeof(Frames));

		for (FFMS_Track::iterator Cur=at(i).begin(); Cur!=at(i).end(); Cur++)
			IndexStream.write(reinterpret_cast<char *>(&*Cur), sizeof(TFrameInfo));
	}
}

void FFMS_Index::ReadIndex(const char *IndexFile) {
	ffms_fstream Index(IndexFile, std::ios::in | std::ios::binary);

	if (!Index.is_open())
		throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_FILE_READ,
			boost::format("Failed to open '%1%' for reading") % IndexFile);
	
	// Read the index file header
	IndexHeader IH;
	Index.read(reinterpret_cast<char *>(&IH), sizeof(IH));
	if (IH.Id != INDEXID)
		throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_INVALID_ARGUMENT,
			boost::format("'%1%' is not a valid index file") % IndexFile);

	if (IH.Version != INDEXVERSION)
		throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_VERSION,
			boost::format("'%1%' is not the expected index version") % IndexFile);

	if (IH.LAVUVersion != avutil_version() || IH.LAVFVersion != avformat_version() ||
		IH.LAVCVersion != avcodec_version() || IH.LSWSVersion != swscale_version() ||
		IH.LPPVersion != postproc_version())
		throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_VERSION,
			boost::format("A different FFmpeg build was used to create '%1%'") % IndexFile);

	if (!(IH.Decoder & FFMS_GetEnabledSources()))
		throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_NOT_AVAILABLE,
			"The source which this index was created with is not available");

	Decoder = IH.Decoder;
	Filesize = IH.FileSize;
	memcpy(Digest, IH.FileSignature, sizeof(Digest));

	try {

		for (unsigned int i = 0; i < IH.Tracks; i++) {
			// Read how many records belong to the current stream
			uint32_t TT;
			Index.read(reinterpret_cast<char *>(&TT), sizeof(TT));
			int64_t Num;
			Index.read(reinterpret_cast<char *>(&Num), sizeof(Num));
			int64_t Den;
			Index.read(reinterpret_cast<char *>(&Den), sizeof(Den));
			int64_t Frames;
			Index.read(reinterpret_cast<char *>(&Frames), sizeof(Frames));
			push_back(FFMS_Track(Num, Den, static_cast<FFMS_TrackType>(TT)));

			TFrameInfo FI = TFrameInfo::VideoFrameInfo(0, 0, false);
			for (size_t j = 0; j < Frames; j++) {
				Index.read(reinterpret_cast<char *>(&FI), sizeof(TFrameInfo));
				at(i).push_back(FI);
			}
		}

	} catch (...) {
		throw FFMS_Exception(FFMS_ERROR_INDEX, FFMS_ERROR_UNKNOWN,
			boost::format("Unknown error while reading index information in '%1%'") % IndexFile);
	}
}

FFMS_Index::FFMS_Index() {
	// this comment documents nothing
}

FFMS_Index::FFMS_Index(int64_t Filesize, uint8_t Digest[20]) {
	this->Filesize = Filesize;
	memcpy(this->Digest, Digest, sizeof(this->Digest));
}

void FFMS_Indexer::SetIndexMask(int IndexMask) {
	this->IndexMask = IndexMask;
}

void FFMS_Indexer::SetDumpMask(int DumpMask) {
	this->DumpMask = DumpMask;
}

void FFMS_Indexer::SetIgnoreDecodeErrors(bool IgnoreDecodeErrors) {
	this->IgnoreDecodeErrors = IgnoreDecodeErrors;
}

void FFMS_Indexer::SetProgressCallback(TIndexCallback IC, void *ICPrivate) {
	this->IC = IC; 
	this->ICPrivate = ICPrivate;
}

void FFMS_Indexer::SetAudioNameCallback(TAudioNameCallback ANC, void *ANCPrivate) {
	this->ANC = ANC;
	this->ANCPrivate = ANCPrivate;
}

FFMS_Indexer *FFMS_Indexer::CreateIndexer(const char *Filename) {
	AVFormatContext *FormatContext = NULL;

	if (av_open_input_file(&FormatContext, Filename, NULL, 0, NULL) != 0)
		throw FFMS_Exception(FFMS_ERROR_PARSER, FFMS_ERROR_FILE_READ,
			boost::format("Can't open '%1%'") % Filename);

	// Do matroska indexing instead?
	if (!strcmp(FormatContext->iformat->name, "matroska")) {
		av_close_input_file(FormatContext);
		return new FFMatroskaIndexer(Filename);
	}

#ifdef HAALISOURCE
	// Do haali ts indexing instead?
	if (HasHaaliMPEG && (!strcmp(FormatContext->iformat->name, "mpeg") || !strcmp(FormatContext->iformat->name, "mpegts"))) {
		av_close_input_file(FormatContext);
		return new FFHaaliIndexer(Filename, FFMS_SOURCE_HAALIMPEG);
	}

	if (HasHaaliOGG && !strcmp(FormatContext->iformat->name, "ogg")) {
		av_close_input_file(FormatContext);
		return new FFHaaliIndexer(Filename, FFMS_SOURCE_HAALIOGG);
	}
#endif

	return new FFLAVFIndexer(Filename, FormatContext);
}

FFMS_Indexer::FFMS_Indexer(const char *Filename) : DecodingBuffer(AVCODEC_MAX_AUDIO_FRAME_SIZE * 5) {
	FFMS_Index::CalculateFileSignature(Filename, &Filesize, Digest);
}

FFMS_Indexer::~FFMS_Indexer() {

}

void FFMS_Indexer::WriteAudio(SharedAudioContext &AudioContext, FFMS_Index *Index, int Track, int DBSize) {
	// Delay writer creation until after an audio frame has been decoded. This ensures that all parameters are known when writing the headers.
	if (DBSize > 0) {
		if (!AudioContext.W64Writer) {
			FFMS_AudioProperties AP;
			FillAP(AP, AudioContext.CodecContext, (*Index)[Track]);
			int FNSize = (*ANC)(SourceFile, Track, &AP, NULL, 0, ANCPrivate);
			std::vector<char> WName(FNSize);
			(*ANC)(SourceFile, Track, &AP, &WName[0], FNSize, ANCPrivate);
			std::string WN(&WName[0]);
			try {
				AudioContext.W64Writer = new Wave64Writer(WN.c_str(), av_get_bits_per_sample_format(AudioContext.CodecContext->sample_fmt),
					AudioContext.CodecContext->channels, AudioContext.CodecContext->sample_rate, (AudioContext.CodecContext->sample_fmt == SAMPLE_FMT_FLT) || (AudioContext.CodecContext->sample_fmt == SAMPLE_FMT_DBL));
			} catch (...) {
				throw FFMS_Exception(FFMS_ERROR_WAVE_WRITER, FFMS_ERROR_FILE_WRITE,
					"Failed to write wave data");
			}
		}

		AudioContext.W64Writer->WriteData(&DecodingBuffer[0], DBSize);
	}
}
