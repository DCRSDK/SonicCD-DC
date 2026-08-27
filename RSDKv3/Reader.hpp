#ifndef READER_H
#define READER_H

#ifdef FORCE_CASE_INSENSITIVE

#include "fcaseopen.h"
#define FileIO                                          FILE
#define fOpen(path, mode)                               fcaseopen(path, mode)
#define fRead(buffer, elementSize, elementCount, file)  fread(buffer, elementSize, elementCount, file)
#define fSeek(file, offset, whence)                     fseek(file, offset, whence)
#define fTell(file)                                     ftell(file)
#define fClose(file)                                    fclose(file)
#define fWrite(buffer, elementSize, elementCount, file) fwrite(buffer, elementSize, elementCount, file)
#define fClearErr(file)                                 clearerr(file)

#else

#if RETRO_USING_SDL2
#define FileIO                                          SDL_RWops
#define fOpen(path, mode)                               SDL_RWFromFile(path, mode)
#define fRead(buffer, elementSize, elementCount, file)  SDL_RWread(file, buffer, elementSize, elementCount)
#define fSeek(file, offset, whence)                     SDL_RWseek(file, offset, whence)
#define fTell(file)                                     SDL_RWtell(file)
#define fClose(file)                                    SDL_RWclose(file)
#define fWrite(buffer, elementSize, elementCount, file) SDL_RWwrite(file, buffer, elementSize, elementCount)
#define fClearErr(file)                                 ((void)0)
#else
#define FileIO                                          FILE
#define fOpen(path, mode)                               fopen(path, mode)
#define fRead(buffer, elementSize, elementCount, file)  fread(buffer, elementSize, elementCount, file)
#define fSeek(file, offset, whence)                     fseek(file, offset, whence)
#define fTell(file)                                     ftell(file)
#define fClose(file)                                    fclose(file)
#define fWrite(buffer, elementSize, elementCount, file) fwrite(buffer, elementSize, elementCount, file)
#define fClearErr(file)                                 clearerr(file)
#endif

#endif

struct FileInfo {
    char fileName[0x100];
    int fileSize;
    int vFileSize;
    int readPos;
    int bufferPosition;
    int virtualFileOffset;
    byte eStringPosA;
    byte eStringPosB;
    byte eStringNo;
    byte eNybbleSwap;
    FileIO *cFileHandle;
    byte *fileBuffer;
#if RETRO_USE_MOD_LOADER
    byte isMod;
#endif
};

extern char rsdkName[0x400];

extern char fileName[0x100];

// Read-ahead buffer size. 0x2000 everywhere except the Dreamcast, where the
// GD-ROM's seek cost dominates load times and a bigger buffer means far fewer
// of them. The FillFileBuffer clamp below keeps the larger size from dragging
// in the bytes of whatever follows a packed entry.
//
// Both this declaration and the definition in Reader.cpp must spell the bound
// the same way — declaring the array at one size and defining it at another is
// a hard error, not a warning.
#if RETRO_USING_KOS
#define FILEBUFFER_SIZE (0x8000)
#else
#define FILEBUFFER_SIZE (0x2000)
#endif
extern byte fileBuffer[FILEBUFFER_SIZE];
extern int fileSize;
extern int vFileSize;
extern int readPos;
extern int readSize;
extern int bufferPosition;
extern int virtualFileOffset;
extern byte eStringPosA;
extern byte eStringPosB;
extern byte eStringNo;
extern byte eNybbleSwap;
extern char encryptionStringA[21];
extern char encryptionStringB[13];
#if RETRO_USE_MOD_LOADER
extern byte isModdedFile;
#endif

extern FileIO *cFileHandle;

#if RETRO_USING_KOS
// Disc traffic, for a load-time report. Two adds per fill, so it costs nothing
// even when nobody is looking, and it turns "the buffer change helped" from an
// assumption into a number.
extern uint dcReadBytes;
extern uint dcReadCalls;

// Report a read that came back short. Out of line so FillFileBuffer stays
// inlineable and the cold path doesn't drag printf into every caller.
void ReportShortFileRead(int got, int want, int at);

// GD-ROM reads are sector-granular. Seeking to a position inside a sector makes
// the driver fetch that whole sector and hand back the tail, and every
// subsequent buffered read then straddles a sector boundary. Landing on the
// sector below and consuming the remainder through the normal read path keeps
// every later refill aligned for the rest of that file. No extra bytes come off
// the disc — the discarded remainder lives inside the refill that was going to
// happen anyway — and the caller ends up at exactly the position it asked for,
// so the bytes delivered are unchanged.
#define DC_CD_SECTOR (2048)
inline void DC_SeekSectorAligned(FileIO *f, int pos)
{
    if (!f)
        return;
    if (pos < 0) { // nothing sensible to align to; behave exactly as before
        fSeek(f, pos, SEEK_SET);
        return;
    }

    const int base = pos & ~(DC_CD_SECTOR - 1);
    fSeek(f, base, SEEK_SET);

    int skip = pos - base;
    while (skip > 0) {
        byte sink[256];
        const int n      = skip > (int)sizeof(sink) ? (int)sizeof(sink) : skip;
        const size_t got = fRead(sink, 1u, (size_t)n, f);
        if (got != (size_t)n) {
            // Short read while skipping: fall back to the plain seek so a
            // failure here can never leave the caller at the wrong offset.
            fClearErr(f);
            fSeek(f, pos, SEEK_SET);
            return;
        }
        skip -= n;
    }
}
#define DC_SEEK_SET(f, p) DC_SeekSectorAligned((f), (p))
#else
#define DC_SEEK_SET(f, p) fSeek((f), (p), SEEK_SET)
#endif

inline void CopyFilePath(char *dest, const char *src)
{
    strcpy(dest, src);
    for (int i = 0;; ++i) {
        if (i >= strlen(dest)) {
            break;
        }

        if (dest[i] == '/')
            dest[i] = '\\';
    }
}
bool CheckRSDKFile(const char *filePath);

bool LoadFile(const char *filePath, FileInfo *fileInfo);
inline bool CloseFile()
{
    int result = 0;
    if (cFileHandle)
        result = fClose(cFileHandle);

    cFileHandle = NULL;
    return result;
}

void FileRead(void *dest, int size);

bool ParseVirtualFileSystem(FileInfo *fileInfo);

inline size_t FillFileBuffer()
{
    // Clamp to the end of the CURRENT file, not of the container. For a packed
    // entry `fileSize` is the whole datapack, so clamping to that alone would
    // let one fill drag in the bytes of whatever follows. At 0x2000 that waste
    // was small; at FILEBUFFER_SIZE a 3KB Act.bin would pull in 29KB of the
    // next file, which is exactly the trade a bigger buffer is meant to win.
    // virtualFileOffset + vFileSize is the entry's end; both are maintained on
    // the loose-file paths too (LoadFile sets them, and SetFileInfo's loose
    // branch was fixed to restore vFileSize), so this is correct either way.
    int fill = (virtualFileOffset + vFileSize) - readPos;
    if (fill > FILEBUFFER_SIZE)
        fill = FILEBUFFER_SIZE;
    if (fill > fileSize - readPos)
        fill = fileSize - readPos;
    if (fill < 0)
        fill = 0;
    readSize = fill;

#if RETRO_USING_KOS
    dcReadBytes += (uint)readSize;
    ++dcReadCalls;

    // fread is allowed to come back short, and the original ignored that:
    // readPos advanced by the full readSize no matter what arrived, so the tail
    // of fileBuffer kept the PREVIOUS fill's bytes while the caller — and, for
    // a packed file, the decryption keystream — carried on as though they were
    // real. Everything read after that point came back as plausible-looking
    // garbage with no error reported anywhere.
    //
    // On the Dreamcast this is not hypothetical: once the music streamer is
    // reading the GD-ROM from its own thread while the main thread reads
    // Data.rsdk, a contended read hands back what it managed to get.
    //
    // readSize is already clamped to what the file physically has left, so a
    // short read here means something went wrong rather than "end of file" — in
    // which case rewind and ask again, since a drive that was busy a moment ago
    // usually isn't a moment later.
    const size_t want = (size_t)(readSize > 0 ? readSize : 0);
    const int at      = readPos;
    size_t got        = 0;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt) {
            // A failed read leaves the stream's error flag set and its position
            // anyone's guess, so clear both before trying the same bytes again.
            fClearErr(cFileHandle);
            DC_SEEK_SET(cFileHandle, at);
            got = 0;
        }

        while (got < want) {
            size_t n = fRead(fileBuffer + got, 1u, want - got, cFileHandle);
            if (!n)
                break;
            got += n;
        }

        if (got >= want)
            break;
    }

    if (got < want) {
        ReportShortFileRead((int)got, (int)want, at);
        memset(fileBuffer + got, 0, want - got);

        // Put the stream where the next fill expects to find it, so one bad
        // read costs one bad buffer rather than every buffer after it.
        fClearErr(cFileHandle);
        DC_SEEK_SET(cFileHandle, at + readSize);
    }

    readPos += readSize;
    bufferPosition = 0;
    return got;
#else
    size_t result = fRead(fileBuffer, 1, readSize, cFileHandle);
    readPos += readSize;
    bufferPosition = 0;
    return result;
#endif
}

inline void GetFileInfo(FileInfo *fileInfo)
{
    StrCopy(fileInfo->fileName, fileName);
    fileInfo->bufferPosition    = bufferPosition;
    fileInfo->readPos           = readPos - readSize;
    fileInfo->fileSize          = fileSize;
    fileInfo->vFileSize         = vFileSize;
    fileInfo->virtualFileOffset = virtualFileOffset;
    fileInfo->eStringPosA       = eStringPosA;
    fileInfo->eStringPosB       = eStringPosB;
    fileInfo->eStringNo         = eStringNo;
    fileInfo->eNybbleSwap       = eNybbleSwap;
#if RETRO_USE_MOD_LOADER
    fileInfo->isMod = isModdedFile;
#endif
}
void SetFileInfo(FileInfo *fileInfo);
size_t GetFilePosition();
void SetFilePosition(int newPos);
bool ReachedEndOfFile();

#endif // !READER_H
