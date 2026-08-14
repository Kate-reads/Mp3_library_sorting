#ifndef TAG_READER_H
#define TAG_READER_H

#ifdef __cplusplus
extern "C" {
    #endif

    int read_mp3_tags(
        const char *filename,
        char *artist,
        int artist_size,
        char *album,
        int album_size,
        char *title,
        int title_size,
        char *year,
        int year_size,
        char *comment,
        int comment_size,
        char *genre,
        int genre_size
    );

    #ifdef __cplusplus
}
#endif

#endif
