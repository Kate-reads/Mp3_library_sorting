#include "tag_reader.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

/*
 * removes whitespace from the end of a string
 *
 * ID3v1 fields have fixed sizes. An artist field is always 30 bytes
 */
static void trim_string(char *string)
{
    int length = strlen(string);

    while (length > 0 &&
    isspace((unsigned char)string[length - 1])) {

            string[length - 1] = '\0';
            length--;
        }
}

/*
 * Copy a fixed sized ID3 field into a normal C string
 * ID3v1 fields:
 * Title: 30 bytes
 * Artist: 30 bytes
 * Album: 30 bytes
 * Year: 4 bytes
 *
 * Normal C strings add '\0' at the end
 *
 * Purpose:
 *
 * detertimes how many bytes to copy
 * prevents buffer overflow
 * copies the bytes
 * adds '\0'
 * removes trailing spaces
 */

static void copy_field(
    char *destination,
    int destination_size,
    const unsigned char *source,
    int source_size
)
{
    int length = source_size;

    if (destination_size <= 0) {
        return;
    }

    memcpy(destination, source, length);

    destination[length] = '\0';

    trim_string(destination);
}
/*
 * Function: read_mp3_tags
 *
 * Parameters:
 * filename - path to mp3 file
 * artist - array where the artist will be stored
 * artist_size - size of artist array
 * album - array where the album name will be stored
 * album_size - size of the album array
 * title - array where the title will be stored
 * title_size - size of the title array
 * year - array where the year will be stored
 * year_size - size of the year array
 * comment - array where comments will be stored
 * comment_size - size of the comment array
 * genre - array where the genre of the track is stored
 * genre_size - size of the genre array
 *
 * Return:
 * 1 = success
 * 0 = failure / no ID3v1 tag
 */
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
)
{
    FILE *file;
    unsigned char tag[128];
    file = fopen(filename, "rb");

    if (file == NULL){
        return 0;
    }

   /*
     * An ID3v1 tag is 128 bytes and is located at the end of the mp3 file
     */

   if (fseek(file, -128L, SEEK_END) != 0) {
       fclose(file);
       return 0;
}

    if (fread(tag , 1, 128, file) != 128) {
        fclose(file);
        return 0;
}

printf("Read bytes: %02X %02X %02X\n",
       tag[0], tag[1], tag[2]);
    fclose(file);
    /*
     * First three bytes are TAG
     * 3..32 == title
     * 33.62 == artist
     * 63..92 == album
     * 93..96 == year
     * 97..126 == comment
     * 127..128 == genre
     */
    if (memcmp(tag, "TAG", 3) != 0){
        return 0;
}
    copy_field(title, title_size, &tag [3], 30);
    copy_field(artist, artist_size, &tag [33], 30);
    copy_field(album, album_size, &tag [63], 30);
    copy_field(year, year_size, &tag [93], 4);
    copy_field(comment, comment_size, &tag [97], 30);
    copy_field(genre, genre_size, &tag [127], 1);

    return 1;

}
