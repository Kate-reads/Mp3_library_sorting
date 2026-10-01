#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif


/*
 * Count MP3 files in a directory
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
        /*
         * Ignore "." and ".."
         */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        char full_path[PATH_MAX];

        int path_length = snprintf(
            full_path,
            sizeof(full_path),
            "%s/%s",
            folder_path,
            entry->d_name
        );

        if (path_length < 0 ||
            (size_t)path_length >= sizeof(full_path))
        {
            continue;
        }

        /*
         * Get file information.
         */
        if (stat(full_path, &file_info) != 0)
        {
            continue;
        }

        /*
         * Only count regular files.
         */
        if (!S_ISREG(file_info.st_mode))
        {
            continue;
        }

        /*
         * Check extension.
         */
        const char *extension =
            strrchr(entry->d_name, '.');

        if (extension != NULL &&
            strcasecmp(extension, ".mp3") == 0)
        {
            number_of_files++;
        }
    }

    closedir(folder);

    return number_of_files;
}


/*
 * Remove whitespace from the end of a string
 */
static void trim_string(char *string)
{
    size_t length;

    if (string == NULL)
    {
        return;
    }

    length = strlen(string);

    while (length > 0 &&
           isspace((unsigned char)string[length - 1]))
    {
        string[length - 1] = '\0';
        length--;
    }
}


/*
 * Copy a fixed-size ID3 field into a normal C string
 */
static void copy_field(
    char *destination,
    int destination_size,
    const unsigned char *source,
    int source_size)
{
    int length;

    if (destination == NULL ||
        source == NULL ||
        destination_size <= 0 ||
        source_size <= 0)
    {
        return;
    }

    length = source_size;

    /*
     * Leave room for '\0'.
     */
    if (length > destination_size - 1)
    {
        length = destination_size - 1;
    }

    memcpy(
        destination,
        source,
        length
    );

    destination[length] = '\0';

    trim_string(destination);
}


/*
 * Basic UTF-16 -> ASCII conversion
 *
 * FF FE = little endian
 * FE FF = big endian
 *
 * Characters outside basic ASCII become '?'
 */
static void copy_utf16_text(
    char *destination,
    int destination_size,
    const unsigned char *source,
    unsigned int source_size)
{
    unsigned int position = 0;
    int output = 0;
    int little_endian = 0;

    if (destination == NULL ||
        source == NULL ||
        destination_size <= 0 ||
        source_size < 2)
    {
        if (destination != NULL &&
            destination_size > 0)
        {
            destination[0] = '\0';
        }

        return;
    }

    /*
     * Check UTF-16 BOM.
     */
    if (source[0] == 0xFF &&
        source[1] == 0xFE)
    {
        little_endian = 1;
        position = 2;
    }
    else if (source[0] == 0xFE &&
             source[1] == 0xFF)
    {
        little_endian = 0;
        position = 2;
    }
    else
    {
        destination[0] = '\0';
        return;
    }

    while (position + 1 < source_size &&
           output < destination_size - 1)
    {
        unsigned short character;

        if (little_endian)
        {
            character =
                (unsigned short)source[position] |
                ((unsigned short)source[position + 1] << 8);
        }
        else
        {
            character =
                ((unsigned short)source[position] << 8) |
                (unsigned short)source[position + 1];
        }

        position += 2;

        /*
         * UTF-16 string terminator.
         */
        if (character == 0x0000)
        {
            break;
        }

        /*
         * Basic ASCII conversion.
         */
        if (character < 128)
        {
            destination[output++] =
                (char)character;
        }
        else
        {
            destination[output++] = '?';
        }
    }

    destination[output] = '\0';
}


/*
 * Read ID3v1 tags
 *
 * Returns:
 *
 * 1 = ID3v1 tag found
 * 0 = no ID3v1 tag / error
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
    int genre_size)
{
    FILE *file;
    unsigned char tag[128];

    file = fopen(filename, "rb");

    if (file == NULL)
    {
        return 0;
    }

    /*
     * ID3v1 tag is the final 128 bytes.
     */
    if (fseek(file, -128L, SEEK_END) != 0)
    {
        fclose(file);
        return 0;
    }

    if (fread(tag, 1, 128, file) != 128)
    {
        fclose(file);
        return 0;
    }

    fclose(file);

    /*
     * ID3v1 must start with "TAG".
     */
    if (memcmp(tag, "TAG", 3) != 0)
    {
        return 0;
    }

    /*
     * ID3v1 layout:
     *
     * 0-2     TAG
     * 3-32    Title
     * 33-62   Artist
     * 63-92   Album
     * 93-96   Year
     * 97-126  Comment
     * 127     Genre
     */
    copy_field(
        title,
        title_size,
        &tag[3],
        30
    );

    copy_field(
        artist,
        artist_size,
        &tag[33],
        30
    );

    copy_field(
        album,
        album_size,
        &tag[63],
        30
    );

    copy_field(
        year,
        year_size,
        &tag[93],
        4
    );

    copy_field(
        comment,
        comment_size,
        &tag[97],
        30
    );


    /*
     * ID3v1 genre table.
     */
    static const char *id3_genres[] =
    {
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

    unsigned char genre_id = tag[127];

    int genre_count =
        sizeof(id3_genres) /
        sizeof(id3_genres[0]);

    if (genre != NULL &&
        genre_size > 0)
    {
        if (genre_id < genre_count)
        {
            snprintf(
                genre,
                genre_size,
                "%s",
                id3_genres[genre_id]
            );
        }
        else
        {
            snprintf(
                genre,
                genre_size,
                "Unknown"
            );
        }
    }

    return 1;
}


/*
 * Read ID3v2.3 tags
 *
 * Supported text frames:
 *
 * TIT2 = title
 * TPE1 = artist
 * TALB = album
 * TYER = year
 * TCON = genre
 *
 * COMM is reserved for later.
 *
 * Returns:
 *
 * 1 = ID3v2.3 tag found
 * 0 = no usable ID3v2.3 tag
 */
static int read_id3v2_tags(
    const char *filename,
    char *artist,
    int artist_size,
    char *album,
    int album_size,
    char *title,
    int title_size,
    char *comment,
    int comment_size,
    char *year,
    int year_size,
    char *genre,
    int genre_size,
    char *disc,
    int disc_size)
{
    /*
     * These are not used yet because
     * COMM parsing has not been implemented.
     */
    (void)comment;
    (void)comment_size;

    FILE *file;
    unsigned char header[10];

    file = fopen(filename, "rb");

    if (file == NULL)
    {
        return 0;
    }

    /*
     * ID3v2 header is the first 10 bytes.
     */
    if (fread(header, 1, 10, file) != 10)
    {
        fclose(file);
        return 0;
    }

    /*
     * First three bytes must be "ID3".
     */
    if (memcmp(header, "ID3", 3) != 0)
    {
        fclose(file);
        return 0;
    }

    /*
     * We currently support ID3v2.3.
     */
    if (header[3] != 3)
    {
        fclose(file);
        return 0;
    }

    /*
     * ID3v2 tag size is a syncsafe integer.
     */
    unsigned int tag_size =
        ((unsigned int)(header[6] & 0x7f) << 21) |
        ((unsigned int)(header[7] & 0x7f) << 14) |
        ((unsigned int)(header[8] & 0x7f) << 7) |
        ((unsigned int)(header[9] & 0x7f));

    printf("ID3v2 tag found!\n");
    printf("Version: 2.%d\n", header[3]);
    printf("Revision: %d\n", header[4]);
    printf("Flags: %02x\n", header[5]);
    printf("Tag Size: %u bytes\n", tag_size);

    if (tag_size == 0)
    {
        fclose(file);
        return 1;
    }

    /*
     * Allocate memory for tag.
     */
    unsigned char *tag_data =
        malloc(tag_size);

    if (tag_data == NULL)
    {
        perror("malloc");
        fclose(file);
        return 0;
    }

    /*
     * Read tag data.
     */
    if (fread(
            tag_data,
            1,
            tag_size,
            file) != tag_size)
    {
        free(tag_data);
        fclose(file);
        return 0;
    }

    fclose(file);

    unsigned int position = 0;

    /*
     * Read each ID3v2.3 frame.
     */
    while (position + 10 <= tag_size)
    {
        char frame_id[5];

        /*
         * Frame ID.
         */
        memcpy(
            frame_id,
            &tag_data[position],
            4
        );

        frame_id[4] = '\0';

        /*
         * Empty frame = padding.
         */
        if (frame_id[0] == '\0')
        {
            break;
        }

        /*
         * Frame size is a normal
         * big-endian 32-bit integer in ID3v2.3.
         */
        unsigned int frame_size =
            ((unsigned int)
                tag_data[position + 4] << 24) |
            ((unsigned int)
                tag_data[position + 5] << 16) |
            ((unsigned int)
                tag_data[position + 6] << 8) |
            ((unsigned int)
                tag_data[position + 7]);

        /*
         * Make sure frame fits inside tag.
         */
        if (position + 10 + frame_size >
            tag_size)
        {
            break;
        }

        printf(
            "Frame %s Size: %u\n",
            frame_id,
            frame_size
        );

        /*
         * Frame data starts after
         * the 10-byte frame header.
         */
        unsigned char *frame_data =
            &tag_data[position + 10];


        /*
         * Text frames.
         */
        if (strcmp(frame_id, "TIT2") == 0 ||
            strcmp(frame_id, "TPE1") == 0 ||
            strcmp(frame_id, "TALB") == 0 ||
            strcmp(frame_id, "TYER") == 0 ||
            strcmp(frame_id, "TCON") == 0)
        {
            if (frame_size > 1)
            {
                /*
                 * First byte is encoding.
                 *
                 * 0 = ISO-8859-1
                 * 1 = UTF-16
                 */
                unsigned char encoding =
                    frame_data[0];

                /*
                 * ISO-8859-1 / ASCII
                 */
                if (encoding == 0)
                {
                    unsigned int text_length =
                        frame_size - 1;

                    char *text =
                        (char *)&frame_data[1];


                    /*
                     * TIT2 = Title
                     */
                    if (strcmp(frame_id, "TIT2") == 0)
                    {
                        unsigned int copy_length =
                            text_length;

                        if (copy_length >=
                            (unsigned int)title_size)
                        {
                            copy_length =
                                title_size - 1;
                        }

                        memcpy(
                            title,
                            text,
                            copy_length
                        );

                        title[copy_length] =
                            '\0';

                        trim_string(title);
                    }


                    /*
                     * TPE1 = Artist
                     */
                    else if (strcmp(frame_id, "TPE1") == 0)
                    {
                        unsigned int copy_length =
                            text_length;

                        if (copy_length >=
                            (unsigned int)artist_size)
                        {
                            copy_length =
                                artist_size - 1;
                        }

                        memcpy(
                            artist,
                            text,
                            copy_length
                        );

                        artist[copy_length] =
                            '\0';

                        trim_string(artist);
                    }


                    /*
                     * TALB = Album
                     */
                    else if (strcmp(frame_id, "TALB") == 0)
                    {
                        unsigned int copy_length =
                            text_length;

                        if (copy_length >=
                            (unsigned int)album_size)
                        {
                            copy_length =
                                album_size - 1;
                        }

                        memcpy(
                            album,
                            text,
                            copy_length
                        );

                        album[copy_length] =
                            '\0';

                        trim_string(album);
                    }


                    /*
                     * TYER = Year
                     */
                    else if (strcmp(frame_id, "TYER") == 0)
                    {
                        unsigned int copy_length =
                            text_length;

                        if (copy_length >=
                            (unsigned int)year_size)
                        {
                            copy_length =
                                year_size - 1;
                        }

                        memcpy(
                            year,
                            text,
                            copy_length
                        );

                        year[copy_length] =
                            '\0';

                        trim_string(year);
                    }


                    /*
                     * TCON = Genre
                     */
                    else if (strcmp(frame_id, "TCON") == 0)
                    {
                        unsigned int copy_length =
                            text_length;

                        if (copy_length >=
                            (unsigned int)genre_size)
                        {
                            copy_length =
                                genre_size - 1;
                        }

                        memcpy(
                            genre,
                            text,
                            copy_length
                        );

                        genre[copy_length] =
                            '\0';

                        trim_string(genre);
                    }
                }


                /*
                 * UTF-16
                 */
                else if (encoding == 1)
                {
                    const unsigned char *text =
                        &frame_data[1];

                    unsigned int text_length =
                        frame_size - 1;


                    /*
                     * TIT2 = Title
                     */
                    if (strcmp(frame_id, "TIT2") == 0)
                    {
                        copy_utf16_text(
                            title,
                            title_size,
                            text,
                            text_length
                        );
                    }


                    /*
                     * TPE1 = Artist
                     */
                    else if (strcmp(frame_id, "TPE1") == 0)
                    {
                        copy_utf16_text(
                            artist,
                            artist_size,
                            text,
                            text_length
                        );
                    }


                    /*
                     * TALB = Album
                     */
                    else if (strcmp(frame_id, "TALB") == 0)
                    {
                        copy_utf16_text(
                            album,
                            album_size,
                            text,
                            text_length
                        );
                    }


                    /*
                     * TYER = Year
                     */
                    else if (strcmp(frame_id, "TYER") == 0)
                    {
                        copy_utf16_text(
                            year,
                            year_size,
                            text,
                            text_length
                        );
                    }


                    /*
                     * TCON = Genre
                     */
                    else if (strcmp(frame_id, "TCON") == 0)
                    {
                        copy_utf16_text(
                            genre,
                            genre_size,
                            text,
                            text_length
                        );
                    }
                }
            }
        }

        /*
         * Move to next frame.
         */
        position += 10 + frame_size;
    }

    free(tag_data);

    return 1;
}


/*
 * Create directory if it doesn't exist
 */
static void create_directory(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
    {
        if (mkdir(path, 0755) != 0)
        {
            perror("mkdir");
        }
    }
    else if (!S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "Path exists but is not a directory: %s\n", path);
    }
}


/*
 * Move file into:
 *
 * base_path/artist/album/song.mp3
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

    int length;


    /*
     * Artist directory
     *
     * base_path/artist
     */
    length = snprintf(
        artist_path,
        sizeof(artist_path),
        "%s/%s",
        base_path,
        artist
    );

    if (length < 0 ||
        (size_t)length >=
            sizeof(artist_path))
    {
        fprintf(
            stderr,
            "Artist path is too long\n"
        );

        return 0;
    }

    create_directory(artist_path);


    /*
     * Album directory
     *
     * base_path/artist/album
     */
    length = snprintf(
        album_path,
        sizeof(album_path),
        "%s/%s",
        artist_path,
        album
    );

    if (length < 0 ||
        (size_t)length >=
            sizeof(album_path))
    {
        fprintf(
            stderr,
            "Album path is too long\n"
        );

        return 0;
    }

    create_directory(album_path);


    /*
     * Destination
     *
     * base_path/artist/album/song.mp3
    */
    length = snprintf(
        destination_path,
        sizeof(destination_path),
        "%s/%s",
        album_path,
        filename
    );

    if (length < 0 ||
        (size_t)length >=
            sizeof(destination_path))
    {
        fprintf(
            stderr,
            "Destination path is too long\n"
        );

        return 0;
    }


    /*
     * Move file.
     */
    if (rename(
            source_path,
            destination_path) != 0)
    {
        perror("rename");
        return 0;
    }

    printf("Moved:\n");
    printf(
        "  %s\n",
        source_path
    );

    printf(
        "  -> %s\n",
        destination_path
    );

    return 1;
}


/*
 * MAIN
 */
int main(void)
{
    /*
     * Folder containing unsorted MP3 files.
     */
    const char *folder_path =
        "/home/kate/Music/";


    /*
     * Count MP3 files
     */
    int number_of_files_in_folder =
        file_counter(folder_path);

    if (number_of_files_in_folder < 0)
    {
        return 1;
    }

    printf(
        "Number of MP3 files: %d\n",
        number_of_files_in_folder
    );


    /*
     * Open directory
     */
    DIR *dir;
    struct dirent *entry;

    dir = opendir(folder_path);

    if (dir == NULL)
    {
        perror("opendir");
        return 1;
    }


    /*
     * Read every directory entry
     */
    while ((entry = readdir(dir)) != NULL)
    {
        /*
         * Ignore "." and "..".
         */
        if (strcmp(entry -> d_name, ".") == 0 ||
            strcmp(entry -> d_name, "..") == 0)
        {
            continue;
        }


        /*
         * Check file extension.
         */
        const char *ext =
            strrchr(entry -> d_name, '.');

        if (ext == NULL ||
            strcasecmp(ext, ".mp3") != 0)
        {
            continue;
        }


        /*
         * Build full file path
         */
        char full_path[PATH_MAX];

        int path_length =
            snprintf(
                full_path,
                sizeof(full_path),
                "%s/%s",
                folder_path,
                entry -> d_name
            );

        if (path_length < 0 ||
            (size_t)path_length >=
                sizeof(full_path))
        {
            fprintf(
                stderr,
                "Path is too long: %s\n",
                entry -> d_name
            );

            continue;
        }


        /*
         * Display file information
         */
        printf("\n");
        printf("-------------------------------\n");

        printf(
            "File: %s\n",
            entry -> d_name
        );

        printf(
            "Path: %s\n",
            full_path
        );


        /*
         * Metadata buffers
         */
        char artist[31] = "";
        char album[31] = "";
        char title[31] = "";
        char year[5] = "";
        char comment[31] = "";
        char genre[64] = "";
        char disc[16] = "";


        /*
         * Try ID3v1 first
         */
        int found =
            read_mp3_tags(
                full_path,

                artist,
                sizeof(artist),

                album,
                sizeof(album),

                title,
                sizeof(title),

                year,
                sizeof(year),

                comment,
                sizeof(comment),

                genre,
                sizeof(genre)
            );


        /*
         * If no ID3v1, try ID3v2.3
         */
        if (!found)
        {
            found =
                read_id3v2_tags(
                    full_path,

                    artist,
                    sizeof(artist),

                    album,
                    sizeof(album),

                    title,
                    sizeof(title),

                    comment,
                    sizeof(comment),

                    year,
                    sizeof(year),

                    genre,
                    sizeof(genre)
                );
        }


        /*
         * Print metadata
         */
        printf("Artist: %s\n", artist);

        printf("Album: %s\n", album);

        printf("Title:  %s\n", title);

        printf("Year: %s\n", year);

        printf("Comment: %s\n", comment);

        printf("Genre: %s\n", genre);

/*
 * Automatically create Artist/Album folders and move the MP3 into the correct album folder.
 */

if (found &&
    artist[0] != '\0' &&
    album[0] != '\0')
{
    printf("Creating music folders...\n");

    if (move_file(
            full_path,
            folder_path,
            artist,
            album,
            entry -> d_name))
    {
        printf("File organized successfully.\n");
    }
    else
    {
        printf("Could not organize file.\n");
    }
}
else
{
    printf("Missing Artist or Album metadata.\n");
    printf("File was not moved.\n");
}

    /*
     * Close directory.
     */
    closedir(dir);

    return 0;
}
