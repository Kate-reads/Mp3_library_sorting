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

/*
 * Count MP3 files
 */

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
        if (strcmp(entry-> d_name, ".") == 0 ||
            strcmp(entry-> d_name, "..") == 0)
        {
            continue;
        }

        /* Create complete file path */
        char full_path[1024];

        snprintf(full_path, sizeof(full_path),
                 "%s/%s", folder_path, entry-> d_name);

        /* Get file information */
        if (stat(full_path, &file_info) != 0)
        {
            continue;
        }

        /* Check if file ends in .mp3 */
        const char *extention = strrchr(entry-> d_name, '.');

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
 *
 * if the artist is "R.E.M this function changes it to "R.E.M"
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

    /*
     * makes sure the destination and source is valid
     */
    if (destination == NULL ||
        source == NULL ||
        destination_size <= 0)
    {
        return;
    }
    /*
     * Start with the size of the ID3v1 feild
     */
    length = source_size;
    /*
     * leave room for '\0'
     * example:
     * destination_size = 31
     * maximum charaters = 30
     */
    if (length > destination_size - 1)
    {
        length = destination_size - 1;
    }

    /*
     * copy ID3v1 bytes into a C string
     */
    memcpy(destination, source, length);

    /*
     * add c string terminator '\0'
     */

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
     * 127 == genre
     */
    if (memcmp(tag, "TAG", 3) != 0){
        return 0;
    }
    copy_field(title, title_size, &tag [3], 30);
    copy_field(artist, artist_size, &tag [33], 30);
    copy_field(album, album_size, &tag [63], 30);
    copy_field(year, year_size, &tag [93], 4);
    copy_field(comment, comment_size, &tag [97], 30);
    /*
     * genres are a number instead of a string
     * tag[127] = 17 = Rock
     */

    static const char *id3_genres[] = {
    "Blues",
    "Classic Rock",
    "Country",
    "Dance",
    "Disco",
    "Funk",
    "Grunge",
    "Hip-Hop",
    "Jazz",
    "Metal",
    "New Age",
    "Oldies",
    "Other",
    "Pop",
    "R&B",
    "Rap",
    "Reggae",
    "Rock",
    "Techno",
    "Industrial",
    "Alternative",
    "Ska",
    "Death Metal",
    "Pranks",
    "Soundtrack",
    "Euro-Techno",
    "Ambient",
    "Trip-Hop",
    "Vocal",
    "Jazz+Funk",
    "Fusion",
    "Trance",
    "Classical",
    "Instrumental",
    "Acid",
    "House",
    "Game",
    "Sound Clip",
    "Gospel",
    "Noise",
    "AlternRock",
    "Bass",
    "Soul",
    "Punk",
    "Space",
    "Meditative",
    "Instrumental Pop",
    "Instrumental Rock",
    "Ethnic",
    "Gothic",
    "Darkwave",
    "Techno-Industrial",
    "Electronic",
    "Pop-Folk",
    "Eurodance",
    "Dream",
    "Southern Rock",
    "Comedy",
    "Cult",
    "Gangsta",
    "Top 40",
    "Christian Rap",
    "Pop/Funk",
    "Jungle",
    "Native American",
    "Cabaret",
    "New Wave",
    "Psychadelic",
    "Rave",
    "Showtunes",
    "Trailer",
    "Lo-Fi",
    "Tribal",
    "Acid Punk",
    "Acid Jazz",
    "Polka",
    "Retro",
    "Musical",
    "Rock & Roll",
    "Hard Rock"
};

    unsigned char genre_id = tag[127]
    /*
     * calculate how man genres are in the ID3v1 table
     */
    int genre_count = sizeof(genre) / sizeof(genre[0]);
    /*
     * check if the genre number exsists
     */
    if (genre_id < genre_count)
    {
        snprintf(genre,
                 genre_size,
                 "%s",
                 genre[genre_id]);
    }
    else
    {
        /*
         * Unknown genre number
         */
        snprintf(genre,
                 genre_size,
                 "Unknown");
    }

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
 * Function: move_file
 * Purpose: move file into new album and artist dirctory base path
 * Returns: base_path/artist/album/song.mp3
 */

static int move_file(
    const char *source_path,
    const char *base_path,
    const char *artist,
    const char *album,
    const char *filename)
{
    char artist_path[PATH_MAX];
    char album_path[PATH_MAX];
    char destination_path[PATH_MAX];


    /*
     * create base_path/artist
     * create artist directory
     */

    snprintf(artist_path, sizeof(artist_path),
             "%s/%s",
             base_path,
             artist);
    create_directory(artist_path);

    /*
     * create base_path/artist/album
     * create album directory
     */

    snprintf(album_path, sizeof(album_path),
             "%s/%s",
             artist_path,
             album);
    create_directory(album_path);


    /*
     * create final destination
     * base_path/artist/album/song.mp3
     */

    snprintf(destination_path, sizeof(destination_path),
             "%s/%s",
             album_path,
             filename);

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
    return 1;
}

int main(void)
{
    /*
     * folder containing unsorted MP3 files
     */
    const char *folder_path = "/home/kate/Music";

    /*
     * count MP3 files
     *
     *
     */
    int number_of_files_in_folder = file_counter(folder_path);

    if (number_of_files_in_folder < 0)
    {
        return 1;
    }

    printf("Number of MP3 files: %d\n", number_of_files_in_folder);

    /*
     *
     * open directory
     */
    DIR *dir;
    struct dirent *entry;

    dir = opendir(folder_path);
    if (dir == NULL) {
        perror("opendir");
        return 1;
    }

    /*
     *
     * read every file
     *
     */
    while ((entry = readdir(dir)) != NULL) {
        /*
         *Skip . and ..
         *
         */
        if (strcmp(entry-> d_name, ".") == 0 ||
            strcmp(entry-> d_name, "..") == 0) {
                continue;
            }
            /*
             * only process mp3 files
             *
             */
            const char *ext = strrchr(entry->d_name, '.');
        if(ext == NULL || strcasecmp(ext, ".mp3") != 0) {
            continue;
        }

        /*
         * create full path to the mp3
         *
         * Example: /home/kate/Music
         */
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path),
                 "%s/%s",
                 folder_path,
                 entry-> d_name);

        /*ID3v1 feilds*/
        char artist[31];
        char album[31];
        char title [31];
        char year [5];
        char comment [31];
        /*genre is now a text string*/
        char genre[32];

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
                   entry-> d_name);
            continue;
        }

        /*
         * display metadata
         */
        printf("\nFile: %s\n", entry-> d_name);
        printf("Artist: %s\n", artist);
        printf("Album: %s\n", album);
        printf("Title: %s\n", title);
        printf("Year: %s\n", year);

        /*move mp3 creates Music/Artist/Album/filename.mp3g*/
        move_file(
            full_path,
            folder_path,
            artist,
            album,
            entry-> d_name
        );

}

closedir(dir);
return 0;
}
