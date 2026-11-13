#pragma once
typedef int SceUID;
#define SCE_O_RDONLY 1
#define SCE_O_WRONLY 2
#define SCE_O_CREAT 4
#define SCE_O_TRUNC 8
SceUID sceIoOpen(const char *, int, int); int sceIoRead(SceUID, void *, unsigned long); int sceIoWrite(SceUID, const void *, unsigned long);
int sceIoClose(SceUID); int sceIoMkdir(const char*, int);
