#include <stdio.h>
#include "tag_reader.h"

int main(void)
{
    char artist[256];
    char album[256];
    char title[256];
    char year[32];
    char comment[256];
    char genre[256];

    const char *filename =
        "/home/kate/Music/Library_2/01 - Runaway.mp3";

    printf("Testing: %s\n", filename);

    if (read_mp3_tags(
            filename,
            artist, sizeof(artist),
            album, sizeof(album),
            title, sizeof(title),
            year, sizeof(year),
            comment, sizeof(comment),
            genre, sizeof(genre)))
    {
        printf("SUCCESS!\n");
        printf("Artist:  %s\n", artist);
        printf("Album:   %s\n", album);
        printf("Title:   %s\n", title);
        printf("Year:    %s\n", year);
        printf("Comment: %s\n", comment);
        printf("Genre:   %s\n", genre);
    }
    else
    {
        printf("Could not read ID3v1 tag.\n");
    }

    return 0;
}
