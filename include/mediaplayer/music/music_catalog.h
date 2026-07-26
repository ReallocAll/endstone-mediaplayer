#ifndef ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_CATALOG_H
#define ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_CATALOG_H

#include "mediaplayer/endstone_api.h"

struct music_catalog {
    char data_dir[ENDSTONE_MEDIAPLAYER_PATH_MAX];
    char nbs_dir[ENDSTONE_MEDIAPLAYER_PATH_MAX];
};

void music_catalog_init(struct music_catalog *catalog, const char *data_dir);
int music_catalog_list(const struct music_catalog *catalog, char ***names);
void music_catalog_free_list(char **names, int count);

#endif // ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_CATALOG_H
