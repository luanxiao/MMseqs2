#ifndef MMSEQS_FILEUTIL_H
#define MMSEQS_FILEUTIL_H

#include <cstdio>
#include <list>
#include <string>

class FileUtil {

public:
    static bool fileExists(const char *fileName);

    static bool directoryExists(const char *directoryName);

    static FILE* openFileOrDie(const char *fileName, const char *mode, bool shouldExist);

    static size_t countLines(const char *name);

    static bool makeDir(const char *dirName, const int mode = 0700);

    static void deleteTempFiles(const std::list<std::string> &tmpFiles);

    static void* mmapFile(FILE * file, size_t *dataSize);

    static void deleteFile(const std::string &tmpFiles);

    static void writeFile(const std::string &pathToFile, const unsigned char *sh, size_t len);

    static std::string dirName(const std::string &file);

    static std::string baseName(const std::string &file);

    static size_t getFreeSpace(const char *dir);

    static void symlinkAlias(const std::string &file, const std::string &alias);
    static void symlinkAbs(const std::string &target, const std::string &link);

    static size_t getFileSize(const std::string &fileName);

    static bool symlinkExists(const std::string &path);

    static void copyFile(const char *src, const char *dst);
};


#endif //MMSEQS_FILEUTIL_H
