/**
 * @file cnc_paths.cpp
 * @brief PathsClass for Retro-Go: locates C&C game data on the SD card.
 */
#include "paths.h"
#include "board_config.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>

static char s_detected_dir[256] = "";

extern "C" void cnc_set_data_dir(const char* dir)
{
    if (dir && *dir) {
        strncpy(s_detected_dir, dir, sizeof(s_detected_dir) - 1);
        s_detected_dir[sizeof(s_detected_dir) - 1] = '\0';
    }
}

extern "C" const char* cnc_get_data_dir(void)
{
    if (s_detected_dir[0] != '\0') {
        return s_detected_dir;
    }

    // Check candidate directories in order of priority
    static const char* candidates[] = {
        "/sd/roms/cnc",
        "/sd/cnc",
        "/sd/cnc_td",
        "/sd/roms/ra",
        "/sd/cnc_ra",
        NULL
    };

    struct stat st;
    for (int i = 0; candidates[i]; i++) {
        char test_file[280];
        snprintf(test_file, sizeof(test_file), "%s/%s", candidates[i], CNC_CHECK_FILE);
        if (stat(test_file, &st) == 0) {
            strncpy(s_detected_dir, candidates[i], sizeof(s_detected_dir) - 1);
            printf("[cnc_paths] Found game data in: %s\n", s_detected_dir);
            return s_detected_dir;
        }
    }

    // Default fallback
    strncpy(s_detected_dir, CNC_DEFAULT_DATA_DIR, sizeof(s_detected_dir) - 1);
    return s_detected_dir;
}

const char* PathsClass::Program_Path()
{
    if (ProgramPath.empty()) {
        ProgramPath = CNC_DATA_DIR;
    }
    return ProgramPath.c_str();
}

const char* PathsClass::Data_Path()
{
    if (DataPath.empty()) {
        DataPath = CNC_DATA_DIR;
    }
    return DataPath.c_str();
}

const char* PathsClass::User_Path()
{
    if (UserPath.empty()) {
        UserPath = CNC_DATA_DIR;
    }
    return UserPath.c_str();
}

bool PathsClass::Create_Directory(const char* dirname)
{
    if (dirname == nullptr) {
        return true;
    }
    std::string temp = dirname;
    size_t pos = 0;
    do {
        pos = temp.find_first_of("/", pos + 1);
        if (mkdir(temp.substr(0, pos).c_str(), 0700) != 0 && errno != EEXIST) {
            return false;
        }
    } while (pos != std::string::npos);
    return true;
}

bool PathsClass::Is_Absolute(const char* path)
{
    return path != nullptr && path[0] == '/';
}

std::string PathsClass::Concatenate_Paths(const char* path1, const char* path2)
{
    if (path1 == nullptr || path1[0] == '\0') {
        return path2 ? std::string(path2) : std::string();
    }
    if (path2 == nullptr || path2[0] == '\0' || strcmp(path2, ".") == 0) {
        return std::string(path1);
    }
    return std::string(path1) + SEP + path2;
}

std::string PathsClass::Get_Filename(const char* path)
{
    const char* name = strrchr(path, '/');
    return std::string(name ? name + 1 : path);
}

std::string PathsClass::Argv_Path(const char* cmd_arg)
{
    std::string ret = cmd_arg;
    size_t sep = ret.find_last_of('/');
    return sep == std::string::npos ? std::string(CNC_DATA_DIR) : ret.substr(0, sep);
}
