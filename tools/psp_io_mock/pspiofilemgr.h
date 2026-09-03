#pragma once

#include <cstdint>

using SceUID = int;

struct SceIoStat
{
    int st_mode;
    std::int64_t st_size;
};

struct SceIoDirent
{
    SceIoStat d_stat;
    char d_name[256];
};

#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT 0x0200
#define PSP_O_TRUNC 0x0400
#define FIO_S_ISDIR(mode) (((mode) & 0x1000) != 0)

extern "C" {
int sceIoGetstat(const char *path, SceIoStat *stat);
SceUID sceIoOpen(const char *path, int flags, int mode);
int sceIoRead(SceUID file, void *data, unsigned int bytes);
int sceIoWrite(SceUID file, const void *data, unsigned int bytes);
int sceIoClose(SceUID file);
int sceIoChdir(const char *path);
int sceIoMkdir(const char *path, int mode);
SceUID sceIoDopen(const char *path);
int sceIoDread(SceUID directory, SceIoDirent *entry);
int sceIoDclose(SceUID directory);
int sceIoSync(const char *device, unsigned int mode);
}
