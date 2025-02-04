// Convert DMD CodeView debug information to PDB files
// Copyright (c) 2009-2010 by Rainer Schuetze, All Rights Reserved
//
// License for redistribution is given by the Artistic License 2.0
// see file LICENSE for further details

#ifndef __PEIMAGE_H__
#define __PEIMAGE_H__

#include "LastError.h"

#include <windows.h>
#include <unordered_map>

struct OMFDirHeader;
struct OMFDirEntry;

struct SymbolInfo
{
	int seg;
	unsigned long off;
	bool dllimport;
};

#define IMGHDR(x) (hdr32 ? hdr32->x : hdr64->x)

class PEImage : public LastError
{
public:
	PEImage(const TCHAR* iname = 0);
	~PEImage();

	template<class P> P* DP(int off) const
	{
		return (P*) ((char*) dump_base + off);
	}
	template<class P> P* DPV(int off, int size) const
	{
		if(off < 0 || off + size > dump_total_len)
			return 0;
		return (P*) ((char*) dump_base + off);
	}
	template<class P> P* DPV(int off) const
	{
		return DPV<P>(off, sizeof(P));
	}
	template<class P> P* CVP(int off) const
	{
		return DPV<P>(cv_base + off, sizeof(P));
	}

	template<class P> P* RVA(unsigned long rva, int len)
	{
		IMAGE_DOS_HEADER *dos = DPV<IMAGE_DOS_HEADER> (0);
		IMAGE_NT_HEADERS32* hdr = DPV<IMAGE_NT_HEADERS32> (dos->e_lfanew);
		IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(hdr);

		for (int i = 0; i < hdr->FileHeader.NumberOfSections; i++)
		{
			if (rva       >= sec[i].VirtualAddress &&
				rva + len <= sec[i].VirtualAddress + sec[i].SizeOfRawData)
				return DPV<P>(sec[i].PointerToRawData + rva - sec[i].VirtualAddress, len);
		}
		return 0;
	}

	bool readAll(const TCHAR* iname);
	bool loadExe(const TCHAR* iname);
	bool loadObj(const TCHAR* iname);
	bool save(const TCHAR* oname);

	bool replaceDebugSection (const void* data, int datalen, bool initCV);
	bool initCVPtr(bool initDbgDir);
	bool initDbgPtr(bool initDbgDir);
	bool initDWARFPtr(bool initDbgDir);
    bool initDWARFObject();
    void initDWARFSegments();
	bool relocateDebugLineInfo(unsigned int img_base);

	bool hasDWARF() const { return debug_line != 0; }
	bool isX64() const { return x64; }
	bool isDBG() const { return dbgfile; }

	int countCVEntries() const;
	OMFDirEntry* getCVEntry(int i) const;

	int getCVSize() const { return dbgDir->SizeOfData; }

	// utilities
	static void* alloc_aligned(unsigned int size, unsigned int align, unsigned int alignoff = 0);
	static void free_aligned(void* p);

	int countSections() const { return nsec; }
	int findSection(unsigned int off) const;
	int findSymbol(const char* name, unsigned long& off, bool& dllimport) const;
	const char* findSectionSymbolName(int s) const;
	const IMAGE_SECTION_HEADER& getSection(int s) const { return sec[s]; }
	unsigned long long getImageBase() const { return IMGHDR(OptionalHeader.ImageBase); }
    int getRelocationInLineSegment(unsigned int offset) const;
    int getRelocationInSegment(int segment, unsigned int offset) const;

    int dumpDebugLineInfoCOFF();
    int dumpDebugLineInfoOMF();
	void createSymbolCache();

private:
	bool _initFromCVDebugDir(IMAGE_DEBUG_DIRECTORY* ddir);

    template<typename SYM> const char* t_findSectionSymbolName(int s) const;

	int fd;
	void* dump_base;
	int dump_total_len;

	// codeview
	IMAGE_DOS_HEADER *dos;
	IMAGE_NT_HEADERS32* hdr32;
	IMAGE_NT_HEADERS64* hdr64;
	IMAGE_SECTION_HEADER* sec;
	IMAGE_DEBUG_DIRECTORY* dbgDir;
	OMFDirHeader* dirHeader;
	OMFDirEntry* dirEntry;
	int nsec;
	int nsym;
	const char* symtable;
	const char* strtable;
	bool x64;     // targets 64-bit machine
	bool bigobj;
	bool dbgfile; // is DBG file
	std::unordered_map<std::string, SymbolInfo> symbolCache;

public:
	//dwarf
	char* debug_aranges;
	char* debug_pubnames;
	char* debug_pubtypes;
	char* debug_info;     unsigned long debug_info_length;
	char* debug_abbrev;   unsigned long debug_abbrev_length;
	char* debug_line;     unsigned long debug_line_length;
	char* debug_line_str; unsigned long debug_line_str_length;
	char* debug_frame;    unsigned long debug_frame_length;
	char* debug_str;
	char* debug_loc;      unsigned long debug_loc_length;
	char* debug_ranges;   unsigned long debug_ranges_length;
	char* reloc;          unsigned long reloc_length;

	int linesSegment;
	int codeSegment;
	int cv_base;
};


#endif //__PEIMAGE_H__
