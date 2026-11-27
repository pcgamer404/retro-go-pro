// SOTN for Retro-Go - boot glue.
//
// Boots Castlevania: Symphony of the Night. Stage selection can be specified
// via /sd/roms/sotn/stage.txt (e.g. "nz0" or "boot").

#include <rg_system.h>
#include <stdio.h>
#include <string.h>

#ifndef SOTN_DATA_ROOT
#error "SOTN_DATA_ROOT must be defined by CMake (see CMakeLists.txt)"
#endif

int sotn_main(int argc, char *argv[]);

const char *sotn_data_root(void)
{
    return SOTN_DATA_ROOT;
}

bool sotn_data_present(void)
{
    if (strncmp(SOTN_DATA_ROOT, RG_STORAGE_ROOT, strlen(RG_STORAGE_ROOT)) != 0)
        RG_LOGW("SOTN_DATA_ROOT (%s) is not under RG_STORAGE_ROOT (%s)!",
                SOTN_DATA_ROOT, RG_STORAGE_ROOT);

    return rg_storage_exists(SOTN_DATA_ROOT "/disks/us/BIN/F_GAME.BIN");
}

void sotn_run(void)
{
    static char stage[8] = "";

    FILE *f = fopen(SOTN_DATA_ROOT "/stage.txt", "r");

    if (f)
    {
        if (fgets(stage, sizeof(stage), f) == NULL)
            stage[0] = '\0';

        for (char *p = stage; *p; p++)
        {
            if (*p == '\r' || *p == '\n')
                *p = '\0';
        }

        fclose(f);
    }

    if (!strcmp(stage, "boot"))
        stage[0] = 0;

    static char *argv[4];
    int argc = 0;

    argv[argc++] = "sotn";

    if (stage[0])
    {
        argv[argc++] = "--stage";
        argv[argc++] = stage;
    }

    argv[argc] = NULL;

    RG_LOGI(
        "Booting stage [%s]",
        stage[0] ? stage : "normal sequence"
    );

    sotn_main(argc, argv);

    RG_LOGI("Game returned");
}
