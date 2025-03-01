/******************************************************************************
 *
 * Project:  CPL - Common Portability Library
 * Purpose:  Standalone shared library that can be LD_PRELOAD'ed as an overload
 *of libc to enable VSI Virtual FILE API to be used with binaries using regular
 *libc for I/O. Author:   Even Rouault <even dot rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

// WARNING: Linux glibc ONLY
// Might work with some adaptations (mainly around 64bit symbols) on other Unix
// systems

// Compile:
// g++ -Wall -fPIC port/vsipreload.cpp -shared -o vsipreload.so -Iport -L.
// -L.libs -lgdal

// Run:
// LD_PRELOAD=./vsipreload.so ....
// e.g:
// LD_PRELOAD=./vsipreload.so gdalinfo
// /vsicurl/http://download.osgeo.org/gdal/data/ecw/spif83.ecw
// LD_PRELOAD=./vsipreload.so gdalinfo
// 'HDF4_EOS:EOS_GRID:"/vsicurl/http://download.osgeo.org/gdal/data/hdf4/MOD09Q1G_EVI.A2006233.h07v03.005.2008338190308.hdf":MODIS_NACP_EVI:MODIS_EVI'
// LD_PRELOAD=./vsipreload.so ogrinfo
// /vsicurl/http://svn.osgeo.org/gdal/trunk/autotest/ogr/data/testavc -ro even
// non GDAL binaries : LD_PRELOAD=./vsipreload.so h5dump -d /x
// /vsicurl/http://download.osgeo.org/gdal/data/netcdf/utm-big-chunks.nc
// LD_PRELOAD=./vsipreload.so sqlite3
// /vsicurl/http://download.osgeo.org/gdal/data/sqlite3/polygon.db "select *
// from polygon limit 10" LD_PRELOAD=./vsipreload.so ls -al
// /vsicurl/http://download.osgeo.org/gdal/data/sqlite3
// LD_PRELOAD=./vsipreload.so find
// /vsicurl/http://download.osgeo.org/gdal/data/sqlite3

/*
 * We need to export open* etc., but _FORTIFY_SOURCE defines conflicting
 * always_inline versions. Disable _FORTIFY_SOURCE for this file, so we
 * can define our overrides.
 */
#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif

#define _GNU_SOURCE 1
#define _LARGEFILE64_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <dlfcn.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <set>
#include <map>
#include <string>
#include "cpl_vsi.h"
#include "cpl_multiproc.h"
#include "cpl_string.h"
#include "cpl_hash_set.h"

static int DEBUG_VSIPRELOAD = 0;
static int DEBUG_VSIPRELOAD_ONLY_VSIL = 1;
#define DEBUG_OUTPUT_READ 0

#ifndef NO_FSTATAT
#define HAVE_FSTATAT
#endif

#define DECLARE_SYMBOL(x, retType, args)                                       \
    typedef retType(*fn##x##Type) args;                                        \
    static fn##x##Type pfn##x = nullptr

DECLARE_SYMBOL(fopen, FILE *, (const char *path, const char *mode));
#ifndef FOPEN64_ALIAS_OF_FOPEN
DECLARE_SYMBOL(fopen64, FILE *, (const char *path, const char *mode));
#endif
DECLARE_SYMBOL(fread, size_t,
               (void *ptr, size_t size, size_t nmemb, FILE *stream));
DECLARE_SYMBOL(fwrite, size_t,
               (const void *ptr, size_t size, size_t nmemb, FILE *stream));
DECLARE_SYMBOL(fclose, int, (FILE * stream));
DECLARE_SYMBOL(__xstat, int, (int ver, const char *path, struct stat *buf));
DECLARE_SYMBOL(__lxstat, int, (int ver, const char *path, struct stat *buf));
DECLARE_SYMBOL(__xstat64, int, (int ver, const char *path, struct stat64 *buf));
#ifndef FSEEKO64_ALIAS_OF_FSEEKO
DECLARE_SYMBOL(fseeko64, int, (FILE * stream, off64_t off, int whence));
#else
DECLARE_SYMBOL(fseeko, int, (FILE * stream, off64_t off, int whence));
#endif
DECLARE_SYMBOL(fseek, int, (FILE * stream, off_t off, int whence));
#ifndef FTELLO64_ALIAS_OF_FTELLO
DECLARE_SYMBOL(ftello64, off64_t, (FILE * stream));
#else
DECLARE_SYMBOL(ftello, off64_t, (FILE * stream));
#endif
DECLARE_SYMBOL(ftell, off_t, (FILE * stream));
DECLARE_SYMBOL(feof, int, (FILE * stream));
DECLARE_SYMBOL(fflush, int, (FILE * stream));
DECLARE_SYMBOL(fgetpos, int, (FILE * stream, fpos_t *pos));
DECLARE_SYMBOL(fsetpos, int, (FILE * stream, fpos_t *pos));
DECLARE_SYMBOL(fileno, int, (FILE * stream));
DECLARE_SYMBOL(ferror, int, (FILE * stream));
DECLARE_SYMBOL(clearerr, void, (FILE * stream));

DECLARE_SYMBOL(fdopen, FILE *, (int fd, const char *mode));
DECLARE_SYMBOL(freopen, FILE *,
               (const char *path, const char *mode, FILE *stream));

DECLARE_SYMBOL(open, int, (const char *path, int flags, mode_t mode));
#ifndef OPEN64_ALIAS_OF_OPEN
DECLARE_SYMBOL(open64, int, (const char *path, int flags, mode_t mode));
#endif
// DECLARE_SYMBOL(creat, int, (const char *path, mode_t mode));
DECLARE_SYMBOL(close, int, (int fd));
DECLARE_SYMBOL(read, ssize_t, (int fd, void *buf, size_t count));
DECLARE_SYMBOL(write, ssize_t, (int fd, const void *buf, size_t count));
DECLARE_SYMBOL(fsync, int, (int fd));
DECLARE_SYMBOL(fdatasync, int, (int fd));
DECLARE_SYMBOL(__fxstat, int, (int ver, int fd, struct stat *__stat_buf));
DECLARE_SYMBOL(__fxstat64, int, (int ver, int fd, struct stat64 *__stat_buf));
#ifdef HAVE_FSTATAT
DECLARE_SYMBOL(__fxstatat, int,
               (int ver, int dirfd, const char *pathname, struct stat *buf,
                int flags));
#endif
#ifdef LSTAT64_ALIAS_OF_LSTAT
DECLARE_SYMBOL(lstat, int, (const char *, struct stat *__stat_buf));
#endif

DECLARE_SYMBOL(lseek, off_t, (int fd, off_t off, int whence));
#ifndef LSEEK64_ALIAS_OF_LSEEK
DECLARE_SYMBOL(lseek64, off64_t, (int fd, off64_t off, int whence));
#endif

DECLARE_SYMBOL(truncate, int, (const char *path, off_t length));
DECLARE_SYMBOL(ftruncate, int, (int fd, off_t length));

DECLARE_SYMBOL(opendir, DIR *, (const char *name));
DECLARE_SYMBOL(readdir, struct dirent *, (DIR * dirp));
#ifndef DIRENT64_ALIAS_OF_DIRENT
DECLARE_SYMBOL(readdir64, struct dirent64 *, (DIR * dirp));
#endif
DECLARE_SYMBOL(closedir, int, (DIR * dirp));
DECLARE_SYMBOL(dirfd, int, (DIR * dirp));
DECLARE_SYMBOL(fchdir, int, (int fd));

static CPLLock *hLock = nullptr;

typedef struct
{
    char *pszDirname;
    char **papszDir;
    int nIter;
    struct dirent ent;
    struct dirent64 ent64;
    int fd;
} VSIDIRPreload;

thread_local std::set<std::string> oSetFilesToPreventRecursion;
std::set<VSILFILE *> oSetFiles;
std::map<int, VSILFILE *> oMapfdToVSI;
std::map<VSILFILE *, int> oMapVSITofd;
std::map<VSILFILE *, std::string> oMapVSIToString;
std::set<VSIDIRPreload *> oSetVSIDIRPreload;
std::map<int, VSIDIRPreload *> oMapfdToVSIDIRPreload;
std::map<int, std::string> oMapDirFdToName;
std::string osCurDir;

/************************************************************************/
/*                             myinit()                                 */
/************************************************************************/

#define LOAD_SYMBOL(x)                                                         \
    pfn##x = reinterpret_cast<fn##x##Type>(dlsym(RTLD_NEXT, #x));              \
    if (pfn##x == nullptr)                                                     \
        fprintf(stderr, "Cannot find symbol %s\n", #x);                        \
    assert(pfn##x)

static void myinit()
{
    CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);

#ifndef FOPEN64_ALIAS_OF_FOPEN
    if (pfnfopen64 != nullptr)
        return;
#else
    if (pfnfopen != nullptr)
        return;
#endif
    DEBUG_VSIPRELOAD = getenv("DEBUG_VSIPRELOAD") != nullptr;
    LOAD_SYMBOL(fopen);
#ifndef FOPEN64_ALIAS_OF_FOPEN
    LOAD_SYMBOL(fopen64);
#endif
    LOAD_SYMBOL(fread);
    LOAD_SYMBOL(fwrite);
    LOAD_SYMBOL(fclose);
#ifndef FSEEKO64_ALIAS_OF_FSEEKO
    LOAD_SYMBOL(fseeko64);
#else
    LOAD_SYMBOL(fseeko);
#endif
    LOAD_SYMBOL(fseek);
    LOAD_SYMBOL(__xstat);
    LOAD_SYMBOL(__lxstat);
    LOAD_SYMBOL(__xstat64);
#ifndef FTELLO64_ALIAS_OF_FTELLO
    LOAD_SYMBOL(ftello64);
#else
    LOAD_SYMBOL(ftello);
#endif
    LOAD_SYMBOL(ftell);
    LOAD_SYMBOL(feof);
    LOAD_SYMBOL(fflush);
    LOAD_SYMBOL(fgetpos);
    LOAD_SYMBOL(fsetpos);
    LOAD_SYMBOL(fileno);
    LOAD_SYMBOL(ferror);
    LOAD_SYMBOL(clearerr);

    LOAD_SYMBOL(fdopen);
    LOAD_SYMBOL(freopen);

    LOAD_SYMBOL(open);
#ifndef OPEN64_ALIAS_OF_OPEN
    LOAD_SYMBOL(open64);
#endif
    // LOAD_SYMBOL(creat);
    LOAD_SYMBOL(close);
    LOAD_SYMBOL(read);
    LOAD_SYMBOL(write);
    LOAD_SYMBOL(fsync);
    LOAD_SYMBOL(fdatasync);
    LOAD_SYMBOL(__fxstat);
    LOAD_SYMBOL(__fxstat64);
#ifdef HAVE_FSTATAT
    LOAD_SYMBOL(__fxstatat);
#endif
#ifdef LSTAT64_ALIAS_OF_LSTAT
    LOAD_SYMBOL(lstat);
#endif
    LOAD_SYMBOL(lseek);
#ifndef LSEEK64_ALIAS_OF_LSEEK
    LOAD_SYMBOL(lseek64);
#endif

    LOAD_SYMBOL(truncate);
    LOAD_SYMBOL(ftruncate);

    LOAD_SYMBOL(opendir);
    LOAD_SYMBOL(readdir);
#ifndef DIRENT64_ALIAS_OF_DIRENT
    LOAD_SYMBOL(readdir64);
#endif
    LOAD_SYMBOL(closedir);
    LOAD_SYMBOL(dirfd);
    LOAD_SYMBOL(fchdir);
}

/************************************************************************/
/*                          getVSILFILE()                               */
/************************************************************************/

static VSILFILE *getVSILFILE(FILE *stream)
{
    CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);
    std::set<VSILFILE *>::iterator oIter =
        oSetFiles.find(reinterpret_cast<VSILFILE *>(stream));
    VSILFILE *ret = nullptr;
    if (oIter != oSetFiles.end())
        ret = *oIter;
    else
        ret = nullptr;
    return ret;
}

/************************************************************************/
/*                          getVSILFILE()                               */
/************************************************************************/

static VSILFILE *getVSILFILE(int fd)
{
    CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);
    std::map<int, VSILFILE *>::iterator oIter = oMapfdToVSI.find(fd);
    VSILFILE *ret = nullptr;
    if (oIter != oMapfdToVSI.end())
        ret = oIter->second;
    else
        ret = nullptr;
    return ret;
}

/************************************************************************/
/*                        VSIFSeekLHelper()                             */
/************************************************************************/

static int VSIFSeekLHelper(VSILFILE *fpVSIL, off64_t off, int whence)
{
    if (off < 0 && whence == SEEK_CUR)
    {
        return VSIFSeekL(fpVSIL, VSIFTellL(fpVSIL) + off, SEEK_SET);
    }
    else if (off < 0 && whence == SEEK_END)
    {
        VSIFSeekL(fpVSIL, 0, SEEK_END);
        return VSIFSeekL(fpVSIL, VSIFTellL(fpVSIL) + off, SEEK_SET);
    }

    return VSIFSeekL(fpVSIL, off, whence);
}

/************************************************************************/
/*                          VSIFopenHelper()                            */
/************************************************************************/

static VSILFILE *VSIFfopenHelper(const char *path, const char *mode)
{
    VSILFILE *fpVSIL = VSIFOpenL(path, mode);
    if (fpVSIL != nullptr)
    {
        CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);
        oSetFiles.insert(fpVSIL);
        oMapVSIToString[fpVSIL] = path;
    }
    return fpVSIL;
}

/************************************************************************/
/*                         getfdFromVSILFILE()                          */
/************************************************************************/

static int getfdFromVSILFILE(VSILFILE *fpVSIL)
{
    CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);

    int fd = 0;
    std::map<VSILFILE *, int>::iterator oIter = oMapVSITofd.find(fpVSIL);
    if (oIter != oMapVSITofd.end())
        fd = oIter->second;
    else
    {
        fd = open("/dev/zero", O_RDONLY);
        assert(fd >= 0);
        oMapVSITofd[fpVSIL] = fd;
        oMapfdToVSI[fd] = fpVSIL;
    }
    return fd;
}

/************************************************************************/
/*                          VSIFopenHelper()                            */
/************************************************************************/

static int VSIFopenHelper(const char *path, int flags)
{
    const char *pszMode = "rb";
    if ((flags & 3) == O_RDONLY)
        pszMode = "rb";
    else if ((flags & 3) == O_WRONLY)
    {
        if (flags & O_APPEND)
            pszMode = "ab";
        else
            pszMode = "wb";
    }
    else
    {
        if (flags & O_APPEND)
            pszMode = "ab+";
        else
            pszMode = "rb+";
    }
    VSILFILE *fpVSIL = VSIFfopenHelper(path, pszMode);
    int fd = 0;
    if (fpVSIL != nullptr)
    {
        if (flags & O_TRUNC)
        {
            VSIFTruncateL(fpVSIL, 0);
            VSIFSeekL(fpVSIL, 0, SEEK_SET);
        }
        fd = getfdFromVSILFILE(fpVSIL);
    }
    else
        fd = -1;
    return fd;
}

/************************************************************************/
/*                    GET_DEBUG_VSIPRELOAD_COND()                             */
/************************************************************************/

static bool GET_DEBUG_VSIPRELOAD_COND(const char *path)
{
    return DEBUG_VSIPRELOAD &&
           // cppcheck-suppress knownConditionTrueFalse
           (!DEBUG_VSIPRELOAD_ONLY_VSIL || STARTS_WITH(path, "/vsi"));
}

static bool GET_DEBUG_VSIPRELOAD_COND(VSILFILE *fpVSIL)
{
    // cppcheck-suppress knownConditionTrueFalse
    return DEBUG_VSIPRELOAD &&
           // cppcheck-suppress knownConditionTrueFalse
           (!DEBUG_VSIPRELOAD_ONLY_VSIL || fpVSIL != nullptr);
}

static bool GET_DEBUG_VSIPRELOAD_COND(VSIDIRPreload *dirP)
{
    return DEBUG_VSIPRELOAD &&
           // cppcheck-suppress knownConditionTrueFalse
           (!DEBUG_VSIPRELOAD_ONLY_VSIL ||
            oSetVSIDIRPreload.find(dirP) != oSetVSIDIRPreload.end());
}

/************************************************************************/
/*                     copyVSIStatBufLToBuf()                           */
/************************************************************************/

static void copyVSIStatBufLToBuf(VSIStatBufL *bufSrc, struct stat *buf)
{
    buf->st_dev = bufSrc->st_dev;
    buf->st_ino = bufSrc->st_ino;
    // S_IXUSR | S_IXGRP | S_IXOTH;
    buf->st_mode = bufSrc->st_mode | S_IRUSR | S_IRGRP | S_IROTH;
    buf->st_nlink = 1;  // bufSrc->st_nlink;
    buf->st_uid = bufSrc->st_uid;
    buf->st_gid = bufSrc->st_gid;
    buf->st_rdev = bufSrc->st_rdev;
    buf->st_size = bufSrc->st_size;
    buf->st_blksize = bufSrc->st_blksize;
    buf->st_blocks = bufSrc->st_blocks;
    buf->st_atime = bufSrc->st_atime;
    buf->st_mtime = bufSrc->st_mtime;
    buf->st_ctime = bufSrc->st_ctime;
}

/************************************************************************/
/*                     copyVSIStatBufLToBuf64()                         */
/************************************************************************/

static void copyVSIStatBufLToBuf64(VSIStatBufL *bufSrc, struct stat64 *buf)
{
    buf->st_dev = bufSrc->st_dev;
    buf->st_ino = bufSrc->st_ino;
    // S_IXUSR | S_IXGRP | S_IXOTH;
    buf->st_mode = bufSrc->st_mode | S_IRUSR | S_IRGRP | S_IROTH;
    buf->st_nlink = 1;  // bufSrc->st_nlink;
    buf->st_uid = bufSrc->st_uid;
    buf->st_gid = bufSrc->st_gid;
    buf->st_rdev = bufSrc->st_rdev;
    buf->st_size = bufSrc->st_size;
    buf->st_blksize = bufSrc->st_blksize;
    buf->st_blocks = bufSrc->st_blocks;
    buf->st_atime = bufSrc->st_atime;
    buf->st_mtime = bufSrc->st_mtime;
    buf->st_ctime = bufSrc->st_ctime;
}

/************************************************************************/
/*                             fopen()                                  */
/************************************************************************/

FILE CPL_DLL *fopen(const char *path, const char *mode)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(path);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fopen(%s, %s)\n", path, mode);
    FILE *ret;
    if (STARTS_WITH(path, "/vsi") && oSetFilesToPreventRecursion.find(path) ==
                                         oSetFilesToPreventRecursion.end())
    {
        auto oIter = oSetFilesToPreventRecursion.insert(path);
        ret = reinterpret_cast<FILE *>(VSIFfopenHelper(path, mode));
        oSetFilesToPreventRecursion.erase(oIter.first);
    }
    else
        ret = pfnfopen(path, mode);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fopen() = %p\n", ret);
    return ret;
}

/************************************************************************/
/*                            fopen64()                                 */
/************************************************************************/

#ifndef FOPEN64_ALIAS_OF_FOPEN
FILE CPL_DLL *fopen64(const char *path, const char *mode)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(path);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fopen64(%s, %s)\n", path, mode);
    FILE *ret;
    if (STARTS_WITH(path, "/vsi") && oSetFilesToPreventRecursion.find(path) ==
                                         oSetFilesToPreventRecursion.end())
    {
        auto oIter = oSetFilesToPreventRecursion.insert(path);
        ret = reinterpret_cast<FILE *>(VSIFfopenHelper(path, mode));
        oSetFilesToPreventRecursion.erase(oIter.first);
    }
    else
        ret = pfnfopen64(path, mode);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fopen64() = %p\n", ret);
    return ret;
}
#endif

/************************************************************************/
/*                            fread()                                   */
/************************************************************************/

size_t CPL_DLL fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fread(stream=%p,size=%d,nmemb=%d)\n", stream,
                static_cast<int>(size), static_cast<int>(nmemb));
    size_t ret = 0;
    if (fpVSIL)
        ret = VSIFReadL(ptr, size, nmemb, fpVSIL);
    else
        ret = pfnfread(ptr, size, nmemb, stream);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fread(stream=%p,size=%d,nmemb=%d) -> %d\n", stream,
                static_cast<int>(size), static_cast<int>(nmemb),
                static_cast<int>(ret));
    return ret;
}

/************************************************************************/
/*                            fwrite()                                  */
/************************************************************************/

size_t CPL_DLL fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fwrite(stream=%p,size=%d,nmemb=%d)\n", stream,
                static_cast<int>(size), static_cast<int>(nmemb));
    size_t ret = 0;
    if (fpVSIL != nullptr)
        ret = VSIFWriteL(ptr, size, nmemb, fpVSIL);
    else
        ret = pfnfwrite(ptr, size, nmemb, stream);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fwrite(stream=%p,size=%d,nmemb=%d) -> %d\n", stream,
                static_cast<int>(size), static_cast<int>(nmemb),
                static_cast<int>(ret));
    return ret;
}

/************************************************************************/
/*                            fclose()                                  */
/************************************************************************/

int CPL_DLL fclose(FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fclose(stream=%p)\n", stream);
    if (fpVSIL != nullptr)
    {
        CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);

        int ret = VSIFCloseL(fpVSIL);
        oMapVSIToString.erase(fpVSIL);
        oSetFiles.erase(fpVSIL);

        std::map<VSILFILE *, int>::iterator oIter = oMapVSITofd.find(fpVSIL);
        if (oIter != oMapVSITofd.end())
        {
            int fd = oIter->second;
            pfnclose(fd);
            oMapVSITofd.erase(oIter);
            oMapfdToVSI.erase(fd);
        }

        return ret;
    }
    else
        return pfnfclose(stream);
}

/************************************************************************/
/*                            __xstat()                                 */
/************************************************************************/

int CPL_DLL __xstat(int ver, const char *path, struct stat *buf)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(path);
    if (DEBUG_VSIPRELOAD && (!osCurDir.empty() && path[0] != '/'))
        DEBUG_VSIPRELOAD_COND = 1;
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "__xstat(%s)\n", path);
    if ((!osCurDir.empty() && path[0] != '/') || STARTS_WITH(path, "/vsi"))
    {
        VSIStatBufL sStatBufL;
        std::string newpath;
        if ((!osCurDir.empty() && path[0] != '/'))
        {
            newpath = CPLFormFilenameSafe(osCurDir.c_str(), path, nullptr);
            path = newpath.c_str();
        }
        const int ret = VSIStatL(path, &sStatBufL);
        sStatBufL.st_ino = static_cast<int>(CPLHashSetHashStr(path));
        if (ret == 0)
        {
            copyVSIStatBufLToBuf(&sStatBufL, buf);
            if (DEBUG_VSIPRELOAD_COND)
                fprintf(stderr, "__xstat(%s) ret = 0, mode = %d, size=%d\n",
                        path, sStatBufL.st_mode,
                        static_cast<int>(sStatBufL.st_size));
        }
        return ret;
    }
    else
    {
        int ret = pfn__xstat(ver, path, buf);
        if (ret == 0)
        {
            if (DEBUG_VSIPRELOAD_COND)
                fprintf(stderr, "__xstat ret = 0, mode = %d\n", buf->st_mode);
        }
        return ret;
    }
}

/************************************************************************/
/*                           __lxstat()                                 */
/************************************************************************/

int CPL_DLL __lxstat(int ver, const char *path, struct stat *buf)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(path);
    if (DEBUG_VSIPRELOAD && (!osCurDir.empty() && path[0] != '/'))
        DEBUG_VSIPRELOAD_COND = 1;
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "__lxstat(%s)\n", path);
    if ((!osCurDir.empty() && path[0] != '/') || STARTS_WITH(path, "/vsi"))
    {
        VSIStatBufL sStatBufL;
        std::string newpath;
        if ((!osCurDir.empty() && path[0] != '/'))
        {
            newpath = CPLFormFilenameSafe(osCurDir.c_str(), path, nullptr);
        }
        else
        {
            newpath = path;
        }
        const int ret = VSIStatL(newpath.c_str(), &sStatBufL);
        sStatBufL.st_ino = static_cast<int>(CPLHashSetHashStr(newpath.c_str()));
        if (ret == 0)
        {
            copyVSIStatBufLToBuf(&sStatBufL, buf);
            if (DEBUG_VSIPRELOAD_COND)
                fprintf(stderr, "__lxstat(%s) ret = 0, mode = %d, size=%d\n",
                        newpath.c_str(), sStatBufL.st_mode,
                        static_cast<int>(sStatBufL.st_size));
        }
        return ret;
    }
    else
    {
        int ret = pfn__lxstat(ver, path, buf);
        if (ret == 0)
        {
            if (DEBUG_VSIPRELOAD_COND)
                fprintf(stderr, "__lxstat ret = 0, mode = %d\n", buf->st_mode);
        }
        return ret;
    }
}

/************************************************************************/
/*                           __xstat64()                                */
/************************************************************************/

int CPL_DLL __xstat64(int ver, const char *path, struct stat64 *buf)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(path);
    if (DEBUG_VSIPRELOAD && (!osCurDir.empty() && path[0] != '/'))
        DEBUG_VSIPRELOAD_COND = 1;
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "__xstat64(%s)\n", path);
    if (((!osCurDir.empty() && path[0] != '/') || STARTS_WITH(path, "/vsi")) &&
        oSetFilesToPreventRecursion.find(path) ==
            oSetFilesToPreventRecursion.end())
    {
        VSIStatBufL sStatBufL;
        std::string newpath;
        if ((!osCurDir.empty() && path[0] != '/'))
        {
            newpath = CPLFormFilenameSafe(osCurDir.c_str(), path, nullptr);
            path = newpath.c_str();
        }
        auto oIter = oSetFilesToPreventRecursion.insert(path);
        const int ret = VSIStatL(path, &sStatBufL);
        oSetFilesToPreventRecursion.erase(oIter.first);
        sStatBufL.st_ino = static_cast<int>(CPLHashSetHashStr(path));
        if (ret == 0)
        {
            copyVSIStatBufLToBuf64(&sStatBufL, buf);
            if (DEBUG_VSIPRELOAD_COND)
                fprintf(stderr, "__xstat64(%s) ret = 0, mode = %d, size = %d\n",
                        path, buf->st_mode, static_cast<int>(buf->st_size));
        }
        return ret;
    }
    else
        return pfn__xstat64(ver, path, buf);
}

/************************************************************************/
/*                           fseeko64()                                 */
/************************************************************************/

#ifndef FSEEKO64_ALIAS_OF_FSEEKO
int CPL_DLL fseeko64(FILE *stream, off64_t off, int whence)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fseeko64(stream=%p, off=%d, whence=%d)\n", stream,
                static_cast<int>(off), whence);
    if (fpVSIL != nullptr)
        return VSIFSeekLHelper(fpVSIL, off, whence);
    else
        return pfnfseeko64(stream, off, whence);
}
#endif

/************************************************************************/
/*                           fseeko()                                 */
/************************************************************************/

int CPL_DLL fseeko(FILE *stream, off_t off, int whence)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fseeko(stream=%p, off=%d, whence=%d)\n", stream,
                static_cast<int>(off), whence);
    if (fpVSIL != nullptr)
        return VSIFSeekLHelper(fpVSIL, off, whence);
    else
    {
#ifndef FSEEKO64_ALIAS_OF_FSEEKO
        return pfnfseeko64(stream, off, whence);
#else
        return pfnfseeko(stream, off, whence);
#endif
    }
}

/************************************************************************/
/*                            fseek()                                   */
/************************************************************************/

int CPL_DLL fseek(FILE *stream, off_t off, int whence)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fseek(stream=%p, off=%d, whence=%d)\n", stream,
                static_cast<int>(off), whence);
    if (fpVSIL != nullptr)
        return VSIFSeekLHelper(fpVSIL, off, whence);
    else
        return pfnfseek(stream, off, whence);
}

/************************************************************************/
/*                           ftello64()                                 */
/************************************************************************/

#ifndef FTELLO64_ALIAS_OF_FTELLO
off64_t CPL_DLL ftello64(FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "ftello64(stream=%p)\n", stream);
    if (fpVSIL != nullptr)
        return VSIFTellL(fpVSIL);
    else
        return pfnftello64(stream);
}
#endif

/************************************************************************/
/*                            ftello()                                  */
/************************************************************************/

off_t CPL_DLL ftello(FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "ftello(stream=%p)\n", stream);
    if (fpVSIL != nullptr)
        return VSIFTellL(fpVSIL);
    else
    {
#ifndef FTELLO64_ALIAS_OF_FTELLO
        return pfnftello64(stream);
#else
        return pfnftello(stream);
#endif
    }
}

/************************************************************************/
/*                            ftell()                                   */
/************************************************************************/

off_t CPL_DLL ftell(FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "ftell(stream=%p)\n", stream);
    if (fpVSIL != nullptr)
        return VSIFTellL(fpVSIL);
    else
        return pfnftell(stream);
}

/************************************************************************/
/*                             feof()                                   */
/************************************************************************/

int CPL_DLL feof(FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "feof(stream=%p)\n", stream);
    if (fpVSIL != nullptr)
        return VSIFEofL(fpVSIL);
    else
        return pfnfeof(stream);
}

/************************************************************************/
/*                            rewind()                                  */
/************************************************************************/

void CPL_DLL rewind(FILE *stream)
{
    fseek(stream, 0, SEEK_SET);
}

/************************************************************************/
/*                            fflush()                                  */
/************************************************************************/

int CPL_DLL fflush(FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fflush(stream=%p)\n", stream);
    if (fpVSIL != nullptr)
        return 0;
    else
        return pfnfflush(stream);
}

/************************************************************************/
/*                            fgetpos()                                 */
/************************************************************************/

int CPL_DLL fgetpos(FILE *stream, fpos_t *pos)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fgetpos(stream=%p)\n", stream);
    if (fpVSIL != nullptr)
    {
        fprintf(stderr, "fgetpos() unimplemented for VSILFILE\n");
        return -1;  // FIXME
    }
    else
        return pfnfgetpos(stream, pos);
}

/************************************************************************/
/*                            fsetpos()                                 */
/************************************************************************/

int CPL_DLL fsetpos(FILE *stream, fpos_t *pos)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fsetpos(stream=%p)\n", stream);
    if (fpVSIL != nullptr)
    {
        fprintf(stderr, "fsetpos() unimplemented for VSILFILE\n");
        return -1;  // FIXME
    }
    else
        return pfnfsetpos(stream, pos);
}

/************************************************************************/
/*                             fileno()                                 */
/************************************************************************/

int CPL_DLL fileno(FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fileno(stream=%p)\n", stream);
    int fd = 0;
    if (fpVSIL != nullptr)
        fd = getfdFromVSILFILE(fpVSIL);
    else
        fd = pfnfileno(stream);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fileno(stream=%p) = %d\n", stream, fd);
    return fd;
}

/************************************************************************/
/*                             ferror()                                 */
/************************************************************************/

int CPL_DLL ferror(FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "ferror(stream=%p)\n", stream);
    if (fpVSIL != nullptr)
    {
        fprintf(stderr, "ferror() unimplemented for VSILFILE\n");
        return 0;  // FIXME ?
    }
    else
        return pfnferror(stream);
}

/************************************************************************/
/*                             clearerr()                               */
/************************************************************************/

void CPL_DLL clearerr(FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "clearerr(stream=%p)\n", stream);
    if (fpVSIL != nullptr)
    {
        fprintf(stderr, "clearerr() unimplemented for VSILFILE\n");
    }
    else
        pfnclearerr(stream);
}

/************************************************************************/
/*                             fdopen()                                 */
/************************************************************************/

FILE CPL_DLL *fdopen(int fd, const char *mode)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(fd);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fdopen(fd=%d)\n", fd);
    if (fpVSIL != nullptr)
    {
        fprintf(stderr, "fdopen() unimplemented for VSILFILE\n");
        return nullptr;  // FIXME ?
    }
    else
        return pfnfdopen(fd, mode);
}

/************************************************************************/
/*                             freopen()                                */
/************************************************************************/

FILE CPL_DLL *freopen(const char *path, const char *mode, FILE *stream)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(stream);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "freopen(path=%s,mode=%s,stream=%p)\n", path, mode,
                stream);
    if (fpVSIL != nullptr)
    {
        fprintf(stderr, "freopen() unimplemented for VSILFILE\n");
        return nullptr;  // FIXME ?
    }
    else
        return pfnfreopen(path, mode, stream);
}

/************************************************************************/
/*                              open()                                  */
/************************************************************************/

static int myopen(const char *path, int flags, ...)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(path);
    if (DEBUG_VSIPRELOAD && !osCurDir.empty() && path[0] != '/')
        DEBUG_VSIPRELOAD_COND = 1;
    if (DEBUG_VSIPRELOAD_COND)
    {
        if (!osCurDir.empty() && path[0] != '/')
            fprintf(
                stderr, "open(%s)\n",
                CPLFormFilenameSafe(osCurDir.c_str(), path, nullptr).c_str());
        else
            fprintf(stderr, "open(%s)\n", path);
    }

    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    int fd = 0;
    if (!osCurDir.empty() && path[0] != '/' && (flags & 3) == O_RDONLY &&
        (flags & O_DIRECTORY) != 0)
    {
        VSIStatBufL sStatBufL;
        char *newname = CPLStrdup(
            CPLFormFilenameSafe(osCurDir.c_str(), path, nullptr).c_str());
        if (strchr(osCurDir.c_str(), '/') != nullptr && strcmp(path, "..") == 0)
        {
            for (int i = 0; i < 2; ++i)
            {
                char *lastslash = strrchr(newname, '/');
                if (!lastslash)
                    break;
                *lastslash = 0;
            }
        }
        if (VSIStatL(newname, &sStatBufL) == 0 && S_ISDIR(sStatBufL.st_mode))
        {
            fd = myopen("/dev/zero", O_RDONLY);
            CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX) oMapDirFdToName[fd] =
                newname;
        }
        else
            fd = -1;
        CPLFree(newname);
    }
    else if (STARTS_WITH(path, "/vsi"))
        fd = VSIFopenHelper(path, flags);
    else
        fd = pfnopen(path, flags, mode);
    va_end(args);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "open(%s) = %d\n", path, fd);
    return fd;
}

int CPL_DLL open(const char *path, int flags, ...)
{
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    int fd = myopen(path, flags, mode);
    va_end(args);
    return fd;
}

/************************************************************************/
/*                             open64()                                 */
/************************************************************************/

#ifndef OPEN64_ALIAS_OF_OPEN
int CPL_DLL open64(const char *path, int flags, ...)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(path);
    if (DEBUG_VSIPRELOAD && !osCurDir.empty() && path[0] != '/')
        DEBUG_VSIPRELOAD_COND = 1;
    if (DEBUG_VSIPRELOAD_COND)
    {
        if (!osCurDir.empty() && path[0] != '/')
            fprintf(
                stderr, "open64(%s)\n",
                CPLFormFilenameSafe(osCurDir.c_str(), path, nullptr).c_str());
        else
            fprintf(stderr, "open64(%s)\n", path);
    }

    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    int fd = 0;
    if (!osCurDir.empty() && path[0] != '/' && (flags & 3) == O_RDONLY &&
        (flags & O_DIRECTORY) != 0)
    {
        VSIStatBufL sStatBufL;
        char *newname = CPLStrdup(
            CPLFormFilenameSafe(osCurDir.c_str(), path, nullptr).c_str());
        if (strchr(osCurDir.c_str(), '/') != nullptr && strcmp(path, "..") == 0)
        {
            for (int i = 0; i < 2; ++i)
            {
                char *lastslash = strrchr(newname, '/');
                if (!lastslash)
                    break;
                *lastslash = 0;
            }
        }
        if (VSIStatL(newname, &sStatBufL) == 0 && S_ISDIR(sStatBufL.st_mode))
        {
            fd = open("/dev/zero", O_RDONLY);
            CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX) oMapDirFdToName[fd] =
                newname;
        }
        else
            fd = -1;
        CPLFree(newname);
    }
    else if (STARTS_WITH(path, "/vsi"))
        fd = VSIFopenHelper(path, flags);
    else
        fd = pfnopen64(path, flags, mode);
    va_end(args);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "open64(%s) = %d\n", path, fd);
    return fd;
}
#endif

/************************************************************************/
/*                             creat()                                  */
/************************************************************************/

int CPL_DLL creat(const char *path, mode_t mode)
{
    return open64(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

/************************************************************************/
/*                             close()                                  */
/************************************************************************/

int CPL_DLL close(int fd)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(fd);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    {
        CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);
        assert(oMapfdToVSIDIRPreload.find(fd) == oMapfdToVSIDIRPreload.end());

        // cppcheck-suppress redundantIfRemove
        if (oMapDirFdToName.find(fd) != oMapDirFdToName.end())
        {
            oMapDirFdToName.erase(fd);
            if (DEBUG_VSIPRELOAD)
                DEBUG_VSIPRELOAD_COND = 1;
        }
    }
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "close(fd=%d)\n", fd);
    if (fpVSIL != nullptr)
    {
        VSIFCloseL(fpVSIL);
        CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);
        oSetFiles.erase(fpVSIL);
        pfnclose(oMapVSITofd[fpVSIL]);
        oMapVSITofd.erase(fpVSIL);
        oMapfdToVSI.erase(fd);
        oMapVSIToString.erase(fpVSIL);
        return 0;
    }
    else
        return pfnclose(fd);
}

/************************************************************************/
/*                              read()                                  */
/************************************************************************/

ssize_t CPL_DLL read(int fd, void *buf, size_t count)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(fd);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "read(fd=%d, count=%d)\n", fd, static_cast<int>(count));
    ssize_t ret = 0;
    if (fpVSIL != nullptr)
        ret = VSIFReadL(buf, 1, count, fpVSIL);
    else
        ret = pfnread(fd, buf, count);
    if (DEBUG_VSIPRELOAD_COND && DEBUG_OUTPUT_READ && ret < 40)
    {
        fprintf(stderr, "read() : ");
        for (int i = 0; i < ret; i++)
        {
            if (static_cast<unsigned char *>(buf)[i] >= 'A' &&
                static_cast<unsigned char *>(buf)[i] <= 'Z')
                fprintf(stderr, "%c ", static_cast<unsigned char *>(buf)[i]);
            else
                fprintf(stderr, "\\%02X ",
                        static_cast<unsigned char *>(buf)[i]);
        }
        fprintf(stderr, "\n");
    }
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "read() -> %d\n", static_cast<int>(ret));
    return ret;
}

/************************************************************************/
/*                              write()                                 */
/************************************************************************/

ssize_t CPL_DLL write(int fd, const void *buf, size_t count)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(fd);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "write(fd=%d, count=%d)\n", fd,
                static_cast<int>(count));
    if (fpVSIL != nullptr)
        return VSIFWriteL(buf, 1, count, fpVSIL);
    else
        return pfnwrite(fd, buf, count);
}

/************************************************************************/
/*                              fsync()                                 */
/************************************************************************/

int CPL_DLL fsync(int fd)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(fd);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fsync(fd=%d)\n", fd);
    if (fpVSIL != nullptr)
        return 0;
    else
        return pfnfsync(fd);
}

/************************************************************************/
/*                           fdatasync()                                */
/************************************************************************/

int CPL_DLL fdatasync(int fd)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(fd);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fdatasync(fd=%d)\n", fd);
    if (fpVSIL != nullptr)
        return 0;
    else
        return pfnfdatasync(fd);
}

/************************************************************************/
/*                            __fxstat()                                */
/************************************************************************/

int CPL_DLL __fxstat(int ver, int fd, struct stat *buf)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(fd);
    std::string name;
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    {
        CPLLockHolderD(&hLock,
                       LOCK_RECURSIVE_MUTEX) if (oMapDirFdToName.find(fd) !=
                                                 oMapDirFdToName.end())
        {
            name = oMapDirFdToName[fd];
            if (DEBUG_VSIPRELOAD)
                DEBUG_VSIPRELOAD_COND = 1;
        }
    }
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "__fxstat(fd=%d)\n", fd);
    if (!name.empty())
    {
        VSIStatBufL sStatBufL;
        if (DEBUG_VSIPRELOAD_COND)
            fprintf(stderr, "__fxstat(%s)\n", name.c_str());
        int ret = VSIStatL(name.c_str(), &sStatBufL);
        sStatBufL.st_ino = static_cast<int>(CPLHashSetHashStr(name.c_str()));
        if (ret == 0)
        {
            copyVSIStatBufLToBuf(&sStatBufL, buf);
            if (DEBUG_VSIPRELOAD_COND)
                fprintf(stderr, "__fxstat ret = 0, mode = %d, size = %d\n",
                        sStatBufL.st_mode, static_cast<int>(sStatBufL.st_size));
        }
        return ret;
    }
    else if (fpVSIL != nullptr)
    {
        VSIStatBufL sStatBufL;
        {
            CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);
            name = oMapVSIToString[fpVSIL];
        }
        int ret = VSIStatL(name.c_str(), &sStatBufL);
        sStatBufL.st_ino = static_cast<int>(CPLHashSetHashStr(name.c_str()));
        if (ret == 0)
        {
            copyVSIStatBufLToBuf(&sStatBufL, buf);
            if (DEBUG_VSIPRELOAD_COND)
                fprintf(stderr, "__fxstat ret = 0, mode = %d, size = %d\n",
                        sStatBufL.st_mode, static_cast<int>(sStatBufL.st_size));
        }
        return ret;
    }
    else
        return pfn__fxstat(ver, fd, buf);
}

/************************************************************************/
/*                           __fxstat64()                               */
/************************************************************************/

int CPL_DLL __fxstat64(int ver, int fd, struct stat64 *buf)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(fd);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "__fxstat64(fd=%d)\n", fd);
    if (fpVSIL != nullptr)
    {
        VSIStatBufL sStatBufL;
        std::string name;
        {
            CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);
            name = oMapVSIToString[fpVSIL];
        }
        int ret = VSIStatL(name.c_str(), &sStatBufL);
        sStatBufL.st_ino = static_cast<int>(CPLHashSetHashStr(name.c_str()));
        if (ret == 0)
        {
            copyVSIStatBufLToBuf64(&sStatBufL, buf);
            if (DEBUG_VSIPRELOAD_COND)
                fprintf(stderr, "__fxstat64 ret = 0, mode = %d, size = %d\n",
                        buf->st_mode, static_cast<int>(buf->st_size));
        }
        return ret;
    }
    else
        return pfn__fxstat64(ver, fd, buf);
}

/************************************************************************/
/*                           __fxstatat()                               */
/************************************************************************/

#ifdef HAVE_FSTATAT
int CPL_DLL __fxstatat(int ver, int dirfd, const char *pathname,
                       struct stat *buf, int flags)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(pathname);
    if (DEBUG_VSIPRELOAD && !osCurDir.empty())
        DEBUG_VSIPRELOAD_COND = 1;
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "__fxstatat(dirfd=%d,pathname=%s,flags=%d)\n", dirfd,
                pathname, flags);

    if (!osCurDir.empty() || STARTS_WITH(pathname, "/vsi"))
    {
        VSIStatBufL sStatBufL;
        std::string osPathname;
        if (!osCurDir.empty() && dirfd == AT_FDCWD && pathname[0] != '/')
            osPathname =
                CPLFormFilenameSafe(osCurDir.c_str(), pathname, nullptr);
        else
            osPathname = pathname;
        const int ret = VSIStatL(osPathname.c_str(), &sStatBufL);
        sStatBufL.st_ino =
            static_cast<int>(CPLHashSetHashStr(osPathname.c_str()));
        if (ret == 0)
        {
            copyVSIStatBufLToBuf(&sStatBufL, buf);
            if (DEBUG_VSIPRELOAD_COND)
                fprintf(stderr,
                        "__fxstatat(%s) ret = 0, mode = %d, size = %d\n",
                        osPathname.c_str(), buf->st_mode,
                        static_cast<int>(buf->st_size));
        }
        return ret;
    }
    else
        return pfn__fxstatat(ver, dirfd, pathname, buf, flags);
}
#endif

/************************************************************************/
/*                             lstat()                                 */
/************************************************************************/

#ifdef LSTAT64_ALIAS_OF_LSTAT
int CPL_DLL lstat(const char *pathname, struct stat *buf)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(pathname);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "lstat(pathname=%s)\n", pathname);

    if (STARTS_WITH(pathname, "/vsi"))
    {
        VSIStatBufL sStatBufL;
        const int ret = VSIStatL(pathname, &sStatBufL);
        sStatBufL.st_ino = static_cast<int>(CPLHashSetHashStr(pathname));
        if (ret == 0)
        {
            copyVSIStatBufLToBuf(&sStatBufL, buf);
            if (DEBUG_VSIPRELOAD_COND)
                fprintf(stderr,
                        "__fxstatat(%s) ret = 0, mode = %d, size = %d\n",
                        pathname, buf->st_mode, static_cast<int>(buf->st_size));
        }
        return ret;
    }
    else
        return pfnlstat(pathname, buf);
}
#endif

/************************************************************************/
/*                              lseek()                                 */
/************************************************************************/

off_t CPL_DLL lseek(int fd, off_t off, int whence)
{
    myinit();
    off_t ret;
    VSILFILE *fpVSIL = getVSILFILE(fd);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "lseek(fd=%d, off=%d, whence=%d)\n", fd,
                static_cast<int>(off), whence);
    if (fpVSIL != nullptr)
    {
        VSIFSeekLHelper(fpVSIL, off, whence);
        ret = VSIFTellL(fpVSIL);
    }
    else
        ret = pfnlseek(fd, off, whence);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "lseek() -> ret = %d\n", static_cast<int>(ret));
    return ret;
}

/************************************************************************/
/*                             lseek64()                                */
/************************************************************************/

#ifndef LSEEK64_ALIAS_OF_LSEEK
off64_t CPL_DLL lseek64(int fd, off64_t off, int whence)
{
    myinit();
    off_t ret;
    VSILFILE *fpVSIL = getVSILFILE(fd);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "lseek64(fd=%d, off=%d, whence=%d)\n", fd,
                static_cast<int>(off), whence);
    if (fpVSIL != nullptr)
    {
        VSIFSeekLHelper(fpVSIL, off, whence);
        ret = VSIFTellL(fpVSIL);
    }
    else
        ret = pfnlseek64(fd, off, whence);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "lseek64() -> ret = %d\n", static_cast<int>(ret));
    return ret;
}
#endif

/************************************************************************/
/*                            truncate()                                */
/************************************************************************/

int CPL_DLL truncate(const char *path, off_t length)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(path);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "truncate(%s)\n", path);

    int ret = 0;
    if (STARTS_WITH(path, "/vsi"))
    {
        VSILFILE *fpVSIL = VSIFOpenL(path, "wb+");
        if (fpVSIL)
        {
            ret = VSIFTruncateL(fpVSIL, length);
            VSIFCloseL(fpVSIL);
        }
        else
            ret = -1;
    }
    else
        ret = pfntruncate(path, length);
    return ret;
}

/************************************************************************/
/*                           ftruncate()                                */
/************************************************************************/

int CPL_DLL ftruncate(int fd, off_t length)
{
    myinit();
    VSILFILE *fpVSIL = getVSILFILE(fd);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(fpVSIL);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "ftruncate(fd=%d)\n", fd);
    int ret = 0;
    if (fpVSIL != nullptr)
    {
        ret = VSIFTruncateL(fpVSIL, length);
    }
    else
        ret = pfnftruncate(fd, length);
    return ret;
}

/************************************************************************/
/*                             opendir()                                */
/************************************************************************/

DIR CPL_DLL *opendir(const char *name)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(name);
    if (DEBUG_VSIPRELOAD && !osCurDir.empty())
        DEBUG_VSIPRELOAD_COND = 1;
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "opendir(%s)\n", name);

    DIR *ret;
    if (!osCurDir.empty() || STARTS_WITH(name, "/vsi"))
    {
        char **papszDir;
        std::string osName;
        if (!osCurDir.empty() && name[0] != '/')
            osName = CPLFormFilenameSafe(osCurDir.c_str(), name, nullptr);
        else
            osName = name;
        papszDir = VSIReadDir(osName.c_str());
        if (papszDir == nullptr)
        {
            VSIStatBufL sStatBufL;
            if (VSIStatL(osName.c_str(), &sStatBufL) == 0 &&
                S_ISDIR(sStatBufL.st_mode))
            {
                papszDir = static_cast<char **>(CPLMalloc(sizeof(char *)));
                papszDir[0] = nullptr;
            }
        }
        if (papszDir == nullptr)
            ret = nullptr;
        else
        {
            VSIDIRPreload *mydir =
                static_cast<VSIDIRPreload *>(malloc(sizeof(VSIDIRPreload)));
            if (!mydir)
            {
                CSLDestroy(papszDir);
                return nullptr;
            }
            mydir->pszDirname = CPLStrdup(osName.c_str());
            mydir->papszDir = papszDir;
            mydir->nIter = 0;
            mydir->fd = -1;
            ret = reinterpret_cast<DIR *>(mydir);
            CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);
            oSetVSIDIRPreload.insert(mydir);
        }
    }
    else
    {
        ret = pfnopendir(name);
    }
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "opendir(%s) -> %p\n", name, ret);
    return ret;
}

/************************************************************************/
/*                             filldir()                                */
/************************************************************************/

static bool filldir(VSIDIRPreload *mydir)
{
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(mydir);
    char *pszName = mydir->papszDir[mydir->nIter++];
    if (pszName == nullptr)
        return false;
    mydir->ent.d_ino = 0;
    mydir->ent.d_off = 0;
    mydir->ent.d_reclen = sizeof(mydir->ent);
    VSIStatBufL sStatBufL;
    CPL_IGNORE_RET_VAL(VSIStatL(
        CPLFormFilenameSafe(mydir->pszDirname, pszName, nullptr).c_str(),
        &sStatBufL));
    if (DEBUG_VSIPRELOAD_COND && S_ISDIR(sStatBufL.st_mode))
        fprintf(stderr, "%s is dir\n", pszName);
    mydir->ent.d_type = S_ISDIR(sStatBufL.st_mode)   ? DT_DIR
                        : S_ISREG(sStatBufL.st_mode) ? DT_REG
                        : S_ISLNK(sStatBufL.st_mode) ? DT_LNK
                                                     : DT_UNKNOWN;
    strncpy(mydir->ent.d_name, pszName, 256);
    mydir->ent.d_name[255] = '\0';

    mydir->ent64.d_ino = 0;
    mydir->ent64.d_off = 0;
    mydir->ent64.d_reclen = sizeof(mydir->ent64);
    mydir->ent64.d_type = mydir->ent.d_type;
    strcpy(mydir->ent64.d_name, mydir->ent.d_name);

    return true;
}

/************************************************************************/
/*                             readdir()                                */
/************************************************************************/

struct dirent CPL_DLL *readdir(DIR *dirp)
{
    myinit();
    VSIDIRPreload *mydir = reinterpret_cast<VSIDIRPreload *>(dirp);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(mydir);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "readdir(%p)\n", dirp);
    if (oSetVSIDIRPreload.find(mydir) != oSetVSIDIRPreload.end())
    {
        if (!filldir(mydir))
            return nullptr;

        return &(mydir->ent);
    }
    else
        return pfnreaddir(dirp);
}

/************************************************************************/
/*                             readdir64()                              */
/************************************************************************/

#ifndef DIRENT64_ALIAS_OF_DIRENT
struct dirent64 CPL_DLL *readdir64(DIR *dirp)
{
    myinit();
    VSIDIRPreload *mydir = reinterpret_cast<VSIDIRPreload *>(dirp);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(mydir);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "readdir64(%p)\n", dirp);
    if (oSetVSIDIRPreload.find(mydir) != oSetVSIDIRPreload.end())
    {
        if (!filldir(mydir))
            return nullptr;

        return &(mydir->ent64);
    }
    else
        return pfnreaddir64(dirp);
}
#endif

/************************************************************************/
/*                             closedir()                               */
/************************************************************************/

int CPL_DLL closedir(DIR *dirp)
{
    myinit();
    VSIDIRPreload *mydir = reinterpret_cast<VSIDIRPreload *>(dirp);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(mydir);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "closedir(%p)\n", dirp);
    if (oSetVSIDIRPreload.find(mydir) != oSetVSIDIRPreload.end())
    {
        CPLFree(mydir->pszDirname);
        CSLDestroy(mydir->papszDir);
        CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);
        if (mydir->fd >= 0)
        {
            oMapfdToVSIDIRPreload.erase(mydir->fd);
            close(mydir->fd);
        }
        oSetVSIDIRPreload.erase(mydir);
        free(mydir);
        return 0;
    }
    else
        return pfnclosedir(dirp);
}

/************************************************************************/
/*                               dirfd()                                */
/************************************************************************/

int CPL_DLL dirfd(DIR *dirp)
{
    myinit();
    VSIDIRPreload *mydir = reinterpret_cast<VSIDIRPreload *>(dirp);
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(mydir);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "dirfd(%p)\n", dirp);
    int ret = 0;
    if (oSetVSIDIRPreload.find(mydir) != oSetVSIDIRPreload.end())
    {
        if (mydir->fd < 0)
        {
            mydir->fd = open("/dev/zero", O_RDONLY);
            CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);
            oMapfdToVSIDIRPreload[mydir->fd] = mydir;
        }
        ret = mydir->fd;
    }
    else
        ret = pfndirfd(dirp);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "dirfd(%p) -> %d\n", dirp, ret);
    return ret;
}

/************************************************************************/
/*                              fchdir()                                */
/************************************************************************/

int CPL_DLL fchdir(int fd)
{
    VSIDIRPreload *mydir = nullptr;
    {
        CPLLockHolderD(&hLock, LOCK_RECURSIVE_MUTEX);
        if (oMapfdToVSIDIRPreload.find(fd) != oMapfdToVSIDIRPreload.end())
            mydir = oMapfdToVSIDIRPreload[fd];
    }
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(mydir);
    std::string name;
    {
        CPLLockHolderD(&hLock,
                       LOCK_RECURSIVE_MUTEX) if (oMapDirFdToName.find(fd) !=
                                                 oMapDirFdToName.end())
        {
            name = oMapDirFdToName[fd];
            if (DEBUG_VSIPRELOAD)
                DEBUG_VSIPRELOAD_COND = 1;
        }
    }
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "fchdir(%d)\n", fd);
    if (!name.empty())
    {
        osCurDir = name;
        if (DEBUG_VSIPRELOAD_COND)
            fprintf(stderr, "fchdir(%d) -> %s\n", fd, osCurDir.c_str());
        return 0;
    }
    else if (mydir != nullptr)
    {
        osCurDir = mydir->pszDirname;
        if (DEBUG_VSIPRELOAD_COND)
            fprintf(stderr, "fchdir(%d) -> %s\n", fd, osCurDir.c_str());
        return 0;
    }
    else
    {
        osCurDir = "";
        if (DEBUG_VSIPRELOAD_COND)
            fprintf(stderr, "fchdir(%d) -> %s\n", fd, osCurDir.c_str());
        return pfnfchdir(fd);
    }
}

/************************************************************************/
/*                        acl_extended_file()                           */
/************************************************************************/

// #include <acl/acl.h>
extern "C" int CPL_DLL acl_extended_file(const char *name);
DECLARE_SYMBOL(acl_extended_file, int, (const char *name));

int acl_extended_file(const char *path)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(path);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "acl_extended_file(%s)\n", path);
    int ret = 0;
    if (STARTS_WITH(path, "/vsi"))
        ret = -1;
    else
    {
        if (pfnacl_extended_file == nullptr)
            pfnacl_extended_file = reinterpret_cast<fnacl_extended_fileType>(
                dlsym(RTLD_NEXT, "acl_extended_file"));
        if (pfnacl_extended_file == nullptr)
            ret = -1;
        else
            ret = pfnacl_extended_file(path);
    }
    return ret;
}

/************************************************************************/
/*                          getfilecon()                                */
/************************************************************************/

// #include <selinux/selinux.h>
extern "C" int CPL_DLL getfilecon(const char *name, void *con);
DECLARE_SYMBOL(getfilecon, int, (const char *name, void *con));

int getfilecon(const char *path, /*security_context_t **/ void *con)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(path);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "getfilecon(%s)\n", path);
    int ret = 0;
    if (STARTS_WITH(path, "/vsi"))
    {
        errno = ENOTSUP;
        ret = -1;
    }
    else
    {
        if (pfngetfilecon == nullptr)
            pfngetfilecon = reinterpret_cast<fngetfileconType>(
                dlsym(RTLD_NEXT, "getfilecon"));
        if (pfngetfilecon == nullptr)
            ret = -1;
        else
            ret = pfngetfilecon(path, con);
    }
    return ret;
}

/************************************************************************/
/*                          lgetfilecon()                                */
/************************************************************************/

// #include <selinux/selinux.h>
extern "C" int CPL_DLL lgetfilecon(const char *name, void *con);
DECLARE_SYMBOL(lgetfilecon, int, (const char *name, void *con));

int lgetfilecon(const char *path, /*security_context_t **/ void *con)
{
    myinit();
    int DEBUG_VSIPRELOAD_COND = GET_DEBUG_VSIPRELOAD_COND(path);
    if (DEBUG_VSIPRELOAD_COND)
        fprintf(stderr, "lgetfilecon(%s)\n", path);
    int ret = 0;
    if (STARTS_WITH(path, "/vsi"))
    {
        errno = ENOTSUP;
        ret = -1;
    }
    else
    {
        if (pfnlgetfilecon == nullptr)
            pfnlgetfilecon = reinterpret_cast<fnlgetfileconType>(
                dlsym(RTLD_NEXT, "lgetfilecon"));
        if (pfnlgetfilecon == nullptr)
            ret = -1;
        else
            ret = pfnlgetfilecon(path, con);
    }
    return ret;
}
