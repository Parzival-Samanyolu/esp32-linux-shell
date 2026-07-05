/* ESP-IDF platform layer for PicoC — replaces platform/platform_unix.c.
 * Routes all interpreter output through a callback (to the TCP shell) and
 * reads source files from the SD card via the normal VFS fopen/stat. */
#include "picoc.h"
#include "interpreter.h"
#include "picoc_esp32.h"

/* Legacy global declared by the original unix platform; kept for compatibility. */
jmp_buf PicocExitBuf;

static picoc_out_fn s_out;
void picoc_set_output(picoc_out_fn fn) { s_out = fn; }
void picoc_emit(char c) { if (s_out) s_out(c); }

void PlatformInit(Picoc *pc) { (void)pc; }
void PlatformCleanup(Picoc *pc) { (void)pc; }

/* No interactive line input on the device (we run files). */
char *PlatformGetLine(char *Buf, int MaxLen, const char *Prompt)
{
    (void)Buf; (void)MaxLen; (void)Prompt;
    return NULL;
}

int PlatformGetCharacter(void) { return -1; }

/* Every character of interpreter output funnels through here. */
void PlatformPutc(unsigned char OutCh, union OutputStreamInfo *Stream)
{
    (void)Stream;
    if (s_out) s_out((char)OutCh);
}

/* Read a whole file from the SD card into a malloc'd buffer. */
char *PlatformReadFile(Picoc *pc, const char *FileName)
{
    struct stat FileInfo;
    if (stat(FileName, &FileInfo))
        ProgramFailNoParser(pc, "can't read file %s\n", FileName);

    char *ReadText = malloc(FileInfo.st_size + 1);
    if (ReadText == NULL)
        ProgramFailNoParser(pc, "out of memory\n");

    FILE *InFile = fopen(FileName, "r");
    if (InFile == NULL)
        ProgramFailNoParser(pc, "can't read file %s\n", FileName);

    int BytesRead = fread(ReadText, 1, FileInfo.st_size, InFile);
    ReadText[BytesRead] = '\0';
    fclose(InFile);
    return ReadText;
}

void PicocPlatformScanFile(Picoc *pc, const char *FileName)
{
    char *SourceStr = PlatformReadFile(pc, FileName);
    if (SourceStr != NULL && SourceStr[0] == '#' && SourceStr[1] == '!') {
        SourceStr[0] = '/';
        SourceStr[1] = '/';
    }
    /* RunIt=true, CleanupNow=false, CleanupSource=true, EnableDebugger=false */
    PicocParse(pc, FileName, SourceStr, strlen(SourceStr), true, false, true, false);
}

void PlatformExit(Picoc *pc, int RetVal)
{
    pc->PicocExitValue = RetVal;
    longjmp(pc->PicocExitBuf, 1);
}

/* No platform-specific interpreted library functions. */
void PlatformLibraryInit(Picoc *pc) { (void)pc; }
