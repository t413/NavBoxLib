#pragma once

/// File class that matches the Arduino File

#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#else
#include <dirent.h>
#include <stdint.h>
#include <stdarg.h>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <string>
#include <filesystem>

namespace fs {

#define FILE_READ       "r"
#define FILE_WRITE      "w"
#define FILE_APPEND     "a"

class File {
    FILE* f = nullptr;
    std::string path_;
    DIR* d = nullptr;
public:
    File() = default;
    File(FILE* file, const char* p = "") : f(file), path_(p) {}
    File(DIR* dir, const char* p = "") : d(dir), path_(p) {}
    ~File() {
        if (f) fclose(f);
        if (d) closedir(d);
    }

    operator bool() const { return f != nullptr; }
    bool seek(long pos) { return f && fseek(f, pos, SEEK_SET) == 0; }
    long size() {
        if (!f) return -1;
        long pos = ftell(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, pos, SEEK_SET);
        return sz;
    }
    int available() { return f ? (size() - position()) : 0; }
    int readBytesUntil(char terminator, char* buffer, size_t length) {
        if (!f) return 0;
        size_t i = 0;
        while (i < length - 1) {
            int c = fgetc(f);
            if (c == EOF) break;
            if (c == terminator) break;
            buffer[i++] = (char)c;
        }
        buffer[i] = '\0';
        return i;
    }
    long position() { return f ? ftell(f) : -1; }
    size_t read(uint8_t* buf, size_t len) { return f ? fread(buf, 1, len, f) : 0; }
    int read() { uint8_t r; return read(&r, 1) == 1? r : -1; }
    size_t readBytes(char *buffer, size_t length) { return read((uint8_t*)buffer, length); }
    size_t write(const uint8_t* buf, size_t len) { return f ? fwrite(buf, 1, len, f) : 0; }
    size_t write(uint8_t c) { return write(&c, 1); }
    int printf(const char* fmt, ...) {
        if (!f) return -1;
        va_list args;
        va_start(args, fmt);
        int ret = vfprintf(f, fmt, args);
        va_end(args);
        return ret;
    }
    int println(const char* s) { return f ? fprintf(f, "%s\n", s) : -1; }

    const char* name() const {
        size_t lastSlash = path_.find_last_of("/\\");
        if (lastSlash == std::string::npos) return path_.c_str();
        return path_.c_str() + lastSlash + 1;
    }

    bool isDirectory() {
        struct stat st;
        if (stat(path_.c_str(), &st) == 0) {
            return S_ISDIR(st.st_mode);
        }
        return false;
    }

    File openNextFile() {
        if (!d) return File();
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            std::string fullPath = path_;
            if (fullPath.back() != '/') fullPath += "/";
            fullPath += entry->d_name;

            struct stat st;
            if (stat(fullPath.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) return File(opendir(fullPath.c_str()), fullPath.c_str());
                return File(fopen(fullPath.c_str(), "r"), fullPath.c_str());
            }
        }
        return File();
    }

    void close() {
        if (f) { fclose(f); f = nullptr; }
        if (d) { closedir(d); d = nullptr; }
    }
};

} //namespace fs

struct SDClass {
    inline fs::File open(const char* path, const char* mode = FILE_READ) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return fs::File(opendir(path), path);
        }
        return fs::File(fopen(path, mode), path);
    }
    bool exists(const char* path) {
        struct stat buffer;
        return (stat(path, &buffer) == 0);
    }
    bool mkdir(const char* path) {
        return ::mkdir(path, 0777) == 0;
    }
};

extern SDClass SD;

uint32_t millis();

#endif
