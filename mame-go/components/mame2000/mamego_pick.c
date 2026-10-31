/*
 * Pick the driver for a ROM zip: the driver named like the zip, or the first
 * one of its family (parent, clones, siblings) whose ROMs are all in the zip.
 * Modern MAME sets often name the parent differently from 0.37b5 (pacman.zip
 * here holds the Midway set, which 0.37b5 calls "pacmanm").
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "driver.h"
#include "unzip.h"

#define MAX_ENTRIES 256

/* 0 = missing, 1 = present by CRC only, 2 = present by name */
static int zip_has(const struct zipent *list, int n, const char *name, uint32_t crc)
{
    int found = 0;
    for (int i = 0; i < n; i++)
    {
        const char *base = strrchr(list[i].name, '/');
        base = base ? base + 1 : list[i].name;
        if (!strcasecmp(base, name))
            return 2;
        if (crc && list[i].crc32 == crc)
            found = 1;
    }
    return found;
}

/* -1 if a required ROM is missing, else the number of ROMs matched by name */
static int roms_score(const struct GameDriver *drv, const struct zipent *list, int n)
{
    const struct RomModule *r = drv->rom;
    int score = 0;
    if (!r)
        return -1;
    for (; r->name || r->offset || r->length; r++)
    {
        if (!r->name || r->name == (char *)-1)
            continue; /* region start, continue or reload */
        int has = zip_has(list, n, r->name, r->crc);
        if (!has && !(r->length & ROMFLAG_OPTIONAL)) /* the flag lives in the length */
            return -1;
        score += has == 2;
    }
    return score;
}

/* The zip picked in the launcher: fileio.c falls back to it when the driver's
 * own <name>.zip does not exist (1942a.zip holding the "1942" set). */
char mamego_zip_path[256];

int mamego_pick_driver(const char *zip_path, const char *base_name)
{
    snprintf(mamego_zip_path, sizeof(mamego_zip_path), "%s", zip_path);

    static struct zipent list[MAX_ENTRIES];
    static char names[MAX_ENTRIES][32];
    int n = 0, exact = -1;

    ZIP *zip = openzip(zip_path);
    if (zip)
    {
        struct zipent *ent;
        while ((ent = readzip(zip)) && n < MAX_ENTRIES)
        {
            list[n] = *ent;
            snprintf(names[n], sizeof(names[n]), "%s", ent->name);
            list[n].name = names[n];
            n++;
        }
        closezip(zip);
    }

    /* Neo Geo sets often leave the BIOS to a separate neogeo.zip (the parent
       set) next to the game: count its files too, the loader finds them there
       through the driver's clone_of chain. */
    {
        char bios[256];
        const char *slash = strrchr(zip_path, '/');
        int dir = slash ? (int)(slash - zip_path) + 1 : 0;
        snprintf(bios, sizeof(bios), "%.*sneogeo.zip", dir, zip_path);
        if (strcasecmp(bios, zip_path) != 0 && (zip = openzip(bios)))
        {
            struct zipent *ent;
            while ((ent = readzip(zip)) && n < MAX_ENTRIES)
            {
                list[n] = *ent;
                snprintf(names[n], sizeof(names[n]), "%s", ent->name);
                list[n].name = names[n];
                n++;
            }
            closezip(zip);
        }
    }

    const struct GameDriver *family = 0;
    for (int i = 0; drivers[i]; i++)
        if (!strcasecmp(drivers[i]->name, base_name))
        {
            exact = i;
            family = drivers[i]->clone_of && !(drivers[i]->clone_of->flags & NOT_A_DRIVER) ? drivers[i]->clone_of : drivers[i];
            break;
        }
    if (exact < 0)
    {
        /* no driver named like the zip: take any complete set it holds */
        int best = -1, best_score = -1;
        for (int i = 0; drivers[i]; i++)
        {
            if (drivers[i]->flags & NOT_A_DRIVER) /* e.g. the Neo Geo BIOS set */
                continue;
            int score = roms_score(drivers[i], list, n);
            if (score > best_score)
                best = i, best_score = score;
        }
        if (best >= 0)
            printf("mamego: %s.zip holds the %s set, using driver %s\n", base_name, drivers[best]->description, drivers[best]->name);
        return best;
    }
    /* the complete set with the most files matched by name wins; ties keep the zip's own name */
    int best = -1, best_score = -1;
    for (int i = 0; drivers[i]; i++)
    {
        const struct GameDriver *d = drivers[i], *a = d;
        /* the family: the set and its clones, down to the modern clones of
           a modern set (shocktro -> shocktrom -> shocktroa) */
        while (a && a != family)
            a = a->clone_of;
        if (!a)
            continue;
        int score = roms_score(d, list, n);
        if (score > best_score || (score == best_score && i == exact))
            best = i, best_score = score;
    }
    if (best < 0)
        return exact; /* let the core report the missing files */
    if (best != exact)
        printf("mamego: %s.zip holds the %s set, using driver %s\n", base_name, drivers[best]->description, drivers[best]->name);
    return best;
}
