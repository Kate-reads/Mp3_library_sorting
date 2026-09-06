#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#include "tag_reader.h" // tag reader

int file_counter(const char *folder_path)
{
    DIR *folder;
    struct dirent *entry;
    struct stat file_info;
    int number_of_files = 0;

    folder = opendir(folder_path);

    if (folder == NULL)
    {
        perror("opendir");
        return -1;
    }

    while ((entry = readdir(folder)) != NULL)
    {
        /* Ignore "." and ".." */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        /* Create complete file path */
        char full_path[1024];

        snprintf(full_path, sizeof(full_path),
                 "%s/%s", folder_path, entry->d_name);

        /* Get file information */
        if (stat(full_path, &file_info) != 0)
        {
            continue;
        }

        /* Check if file ends in .mp3 */
        const char *extention = strrchr(entry->d_name, '.');

        if (extention != NULL &&
            strcasecmp(extention, ".mp3") == 0)
        {
            number_of_files++;
        }
    }
    closedir(folder);

    return number_of_files;
}

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
    int source_size)

{
    int length;

    if (destination == NULL || destination_size <= 0)
    {
        return;
    }

    length = source_path;
    /*
     * leave room for '\0'
     */
    if (length > destination_size - 1)
    {
        length = destination_size - 1;
    }
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

/*
 * Function: create_directory
 *
 * Purpose: creates a new folder directory if that directory doesn't already exsist based on the MP3 ID3 metadata
 *
 * Returns: new file path
 */

static void create_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        if  (mkdir(path, 0755) != 0) {
            perror("mkdir");
        }
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "This exsists but not in the directory\n", path);
    }
}


/*
 * Function: artist directory
 * Purpose: creates file directory to an artist folder from metadata
 */


static void create_artist_directory(const char *base_path, const char *artist)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", base_path, artist);
    create_directory(path);
}

/*
 * Function: album directory
 * Purpose: creates a file directory to a new album folder within the artist directory
 */

static void crate_album_directory(const char *base_path, const char *album)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", base_path, album);

}

/*
 * Function: move_file
 * Purpose: move file into new album and artist dirctory base path
 */

static int move_file(
    const char *source_path,
    const char *base_path,
    const char *artist,
    const char *album,
    const char *filename)
{
    char safe_artist[256];
    char safe_album[256];

    char artist_path[PATH_MAX];
    char album_path[PATH_MAX];
    char destination_path[PATH_MAX];


    /*
     * create base_path/artist
     */

    snprintf(artist_path, sizeof(artist_path),
             "%s/%s",
             base_path,
             artist);
    create_directory(artist_path);

    /*
     * create base_path/artist/album
     */

    snprintf(album_path, sizeof(album_path),
             "%s/%s",
             base_path,
             album);
    create_directory(album_path);


    /*
     * create final destination
     * base_path/artist/album/song.mp3
     */

    snprintf(destination_path, (sizeof(destination_path,
                                "%s/%s",
                                album_path,
                                filename)));

    /*
     * move file
     */

    if (rename(source_path, destination_path) != 0)
    {
        perror("rename");
        return 0;
    }
    printf("moved\n");
    printf(" %s\n", source_path);
    printf(" -> %s\n", destination_path);
    return;

int main(void)
{
    const char *folder_path = "/home/kate/Music/Library_2";

    int number_of_files_in_folder = file_counter(folder_path);

    if (number_of_files_in_folder < 0)
    {
        return 1;
    }

    printf("Number of MP3 files: %d\n", number_of_files_in_folder);

    DIR *dir;
    struct dirent *entry;

    dir = opendir(folder_path);
    if (dir == NULL) {
        perror("opendir");
        return 1;
    }

    while ((entry = readdir(dir)) != NULL) {
        /*Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            /* only process mp3 files */
            const char *ext = strrchr(entry->d_name, '.');
        if(ext == NULL || strcasecmp(ext, ".mp3") != 0) {
            continue;
        }

        /*create full path to the mp3*/
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path),
                 "%s/%s",
                 folder_path,
                 entry->d_name);

        /*ID3v1 feilds*/
        char artist[31];
        char album[31];
        char title [31];
        char year [5];
        char comment [31];
        char genre[2];

        /*read metadata*/
        if(!read_mp3_tags(
            full_path,
            artist, sizeof(artist),
            album, sizeof(album),
            title, sizeof(title),
            year, sizeof(year),
            comment, sizeof(comment),
            genre, sizeof(genre)))
        {
            printf("Skipping %s: no ID tags\n",
                   entry->d_name);
            continue;
        }

        printf("\nFile: %s\n", entry->d_name);
        printf("Artist: %s\n", artist);
        printf("Album: %s\n", album);
        printf("Title: %s\n", title);
        printf("Year: %s\n", year);

        /*move mp3*/
        move_file(
            full_path,
            folder_path,
            artist,
            album,
            entry->d_name
        );

}

closedir(dir);
return 0;
}
