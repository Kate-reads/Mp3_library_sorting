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

#define MAX_TRACKS 1000
#define MAX_TEXT 256
#define ID3_HEADER_SIZE 10

struct music_file
{
    char source_path[PATH_MAX];
    char filename[PATH_MAX];

    char artist[MAX_TEXT];
    char album[MAX_TEXT];
    char title[MAX_TEXT];

    int disc_number;
    int track_number;
};

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

    while (*string != '\0' &&
           isspace((unsigned char)*string))
    {
        memmove(
            string,
            string + 1,
            strlen(string)
        );
    }
}

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

        if (character == 0x0000)
        {
            break;
        }

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

    trim_string(destination);
}

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
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        char full_path[PATH_MAX];

        int path_length =
            snprintf(
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

        if (stat(full_path, &file_info) != 0)
        {
            continue;
        }

        if (!S_ISREG(file_info.st_mode))
        {
            continue;
        }

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

    if (memcmp(tag, "TAG", 3) != 0)
    {
        return 0;
    }

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
    int *track_number,
    int *disc_number)
{
    (void)comment;
    (void)comment_size;

    FILE *file;
    unsigned char header[10];

    file = fopen(filename, "rb");

    if (file == NULL)
    {
        return 0;
    }

    if (fread(header, 1, 10, file) != 10)
    {
        fclose(file);
        return 0;
    }

    if (memcmp(header, "ID3", 3) != 0)
    {
        fclose(file);
        return 0;
    }

    if (header[3] != 3)
    {
        fclose(file);
        return 0;
    }

    unsigned int tag_size =
        ((unsigned int)(header[6] & 0x7f) << 21) |
        ((unsigned int)(header[7] & 0x7f) << 14) |
        ((unsigned int)(header[8] & 0x7f) << 7) |
        ((unsigned int)(header[9] & 0x7f));

    if (tag_size == 0)
    {
        fclose(file);
        return 1;
    }

    unsigned char *tag_data =
        malloc(tag_size);

    if (tag_data == NULL)
    {
        perror("malloc");
        fclose(file);
        return 0;
    }

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

    while (position + 10 <= tag_size)
    {
        char frame_id[5];

        memcpy(
            frame_id,
            &tag_data[position],
            4
        );

        frame_id[4] = '\0';

        if (frame_id[0] == '\0')
        {
            break;
        }

        unsigned int frame_size =
            ((unsigned int)
                tag_data[position + 4] << 24) |
            ((unsigned int)
                tag_data[position + 5] << 16) |
            ((unsigned int)
                tag_data[position + 6] << 8) |
            ((unsigned int)
                tag_data[position + 7]);

        if (frame_size == 0)
        {
            position += 10;
            continue;
        }

        if (position + 10 + frame_size >
            tag_size)
        {
            break;
        }

        unsigned char *frame_data =
            &tag_data[position + 10];

        if (strcmp(frame_id, "TIT2") == 0 ||
            strcmp(frame_id, "TPE1") == 0 ||
            strcmp(frame_id, "TALB") == 0 ||
            strcmp(frame_id, "TYER") == 0 ||
            strcmp(frame_id, "TCON") == 0 ||
            strcmp(frame_id, "TRCK") == 0 ||
            strcmp(frame_id, "TPOS") == 0)
        {
            if (frame_size > 1)
            {
                unsigned char encoding =
                    frame_data[0];

                if (encoding == 0)
                {
                    char text[MAX_TEXT];

                    unsigned int text_length =
                        frame_size - 1;

                    unsigned int copy_length =
                        text_length;

                    if (copy_length >= sizeof(text))
                    {
                        copy_length =
                            sizeof(text) - 1;
                    }

                    memcpy(
                        text,
                        &frame_data[1],
                        copy_length
                    );

                    text[copy_length] = '\0';

                    trim_string(text);

                    if (strcmp(frame_id, "TIT2") == 0)
                    {
                        snprintf(
                            title,
                            title_size,
                            "%s",
                            text
                        );
                    }
                    else if (strcmp(frame_id, "TPE1") == 0)
                    {
                        snprintf(
                            artist,
                            artist_size,
                            "%s",
                            text
                        );
                    }
                    else if (strcmp(frame_id, "TALB") == 0)
                    {
                        snprintf(
                            album,
                            album_size,
                            "%s",
                            text
                        );
                    }
                    else if (strcmp(frame_id, "TYER") == 0)
                    {
                        snprintf(
                            year,
                            year_size,
                            "%s",
                            text
                        );
                    }
                    else if (strcmp(frame_id, "TCON") == 0)
                    {
                        snprintf(
                            genre,
                            genre_size,
                            "%s",
                            text
                        );
                    }
                    else if (strcmp(frame_id, "TRCK") == 0)
                    {
                        *track_number =
                            atoi(text);
                    }
                    else if (strcmp(frame_id, "TPOS") == 0)
                    {
                        *disc_number =
                            atoi(text);
                    }
                }
                else if (encoding == 1)
                {
                    char text[MAX_TEXT];

                    copy_utf16_text(
                        text,
                        sizeof(text),
                        &frame_data[1],
                        frame_size - 1
                    );

                    if (strcmp(frame_id, "TIT2") == 0)
                    {
                        snprintf(
                            title,
                            title_size,
                            "%s",
                            text
                        );
                    }
                    else if (strcmp(frame_id, "TPE1") == 0)
                    {
                        snprintf(
                            artist,
                            artist_size,
                            "%s",
                            text
                        );
                    }
                    else if (strcmp(frame_id, "TALB") == 0)
                    {
                        snprintf(
                            album,
                            album_size,
                            "%s",
                            text
                        );
                    }
                    else if (strcmp(frame_id, "TYER") == 0)
                    {
                        snprintf(
                            year,
                            year_size,
                            "%s",
                            text
                        );
                    }
                    else if (strcmp(frame_id, "TCON") == 0)
                    {
                        snprintf(
                            genre,
                            genre_size,
                            "%s",
                            text
                        );
                    }
                    else if (strcmp(frame_id, "TRCK") == 0)
                    {
                        *track_number =
                            atoi(text);
                    }
                    else if (strcmp(frame_id, "TPOS") == 0)
                    {
                        *disc_number =
                            atoi(text);
                    }
                }
            }
        }

        position += 10 + frame_size;
    }

    free(tag_data);

    return 1;
}

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
        fprintf(
            stderr,
            "Path exists but is not a directory: %s\n",
            path
        );
    }
}

static void sanitize_filename(char *string)
{
    size_t i;

    if (string == NULL)
    {
        return;
    }

    for (i = 0; string[i] != '\0'; i++)
    {
        if (string[i] == '/' ||
            string[i] == '\\' ||
            string[i] == ':' ||
            string[i] == '*' ||
            string[i] == '?' ||
            string[i] == '"' ||
            string[i] == '<' ||
            string[i] == '>' ||
            string[i] == '|')
        {
            string[i] = '_';
        }
    }

    trim_string(string);
}

static int compare_music_files(
    const void *a,
    const void *b)
{
    const struct music_file *file_a =
        (const struct music_file *)a;

    const struct music_file *file_b =
        (const struct music_file *)b;

    if (file_a->disc_number !=
        file_b->disc_number)
    {
        return
            file_a->disc_number -
            file_b->disc_number;
    }

    if (file_a->track_number !=
        file_b->track_number)
    {
        return
            file_a->track_number -
            file_b->track_number;
    }

    return strcmp(
        file_a->filename,
        file_b->filename
    );
}

static int move_file(
    const char *source_path,
    const char *base_path,
    const char *artist,
    const char *album,
    const char *filename,
    char *destination,
    size_t destination_size)
{
    char artist_path[PATH_MAX];
    char album_path[PATH_MAX];

    int length;

    length = snprintf(
        artist_path,
        sizeof(artist_path),
        "%s/%s",
        base_path,
        artist
    );

    if (length < 0 ||
        (size_t)length >= sizeof(artist_path))
    {
        fprintf(
            stderr,
            "Artist path is too long\n"
        );

        return 0;
    }

    create_directory(artist_path);

    length = snprintf(
        album_path,
        sizeof(album_path),
        "%s/%s",
        artist_path,
        album
    );

    if (length < 0 ||
        (size_t)length >= sizeof(album_path))
    {
        fprintf(
            stderr,
            "Album path is too long\n"
        );

        return 0;
    }

    create_directory(album_path);

    length = snprintf(
        destination,
        destination_size,
        "%s/%s",
        album_path,
        filename
    );

    if (length < 0 ||
        (size_t)length >= destination_size)
    {
        fprintf(
            stderr,
            "Destination path is too long\n"
        );

        return 0;
    }

    if (rename(
            source_path,
            destination) != 0)
    {
        perror("rename");
        return 0;
    }

    printf(
        "Moved:\n  %s\n  -> %s\n",
        source_path,
        destination
    );

    return 1;
}

static int read_binary_file(
    const char *filename,
    unsigned char **data,
    size_t *size)
{
    FILE *file;
    long file_size;

    *data = NULL;
    *size = 0;

    file = fopen(filename, "rb");

    if (file == NULL)
    {
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return 0;
    }

    file_size = ftell(file);

    if (file_size < 0)
    {
        fclose(file);
        return 0;
    }

    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return 0;
    }

    if (file_size == 0)
    {
        fclose(file);
        return 0;
    }

    *data = malloc((size_t)file_size);

    if (*data == NULL)
    {
        fclose(file);
        return 0;
    }

    if (fread(
            *data,
            1,
            (size_t)file_size,
            file) != (size_t)file_size)
    {
        free(*data);
        *data = NULL;
        fclose(file);
        return 0;
    }

    fclose(file);

    *size = (size_t)file_size;

    return 1;
}

static void write_synchsafe(
    unsigned char output[4],
    unsigned int value)
{
    output[0] =
        (unsigned char)((value >> 21) & 0x7F);

    output[1] =
        (unsigned char)((value >> 14) & 0x7F);

    output[2] =
        (unsigned char)((value >> 7) & 0x7F);

    output[3] =
        (unsigned char)(value & 0x7F);
}

static int find_id3v2_size(
    FILE *file,
    unsigned int *tag_size)
{
    unsigned char header[10];

    if (fseek(file, 0, SEEK_SET) != 0)
    {
        return 0;
    }

    if (fread(header, 1, 10, file) != 10)
    {
        return 0;
    }

    if (memcmp(header, "ID3", 3) != 0)
    {
        *tag_size = 0;
        return 1;
    }

    *tag_size =
        ((unsigned int)(header[6] & 0x7F) << 21) |
        ((unsigned int)(header[7] & 0x7F) << 14) |
        ((unsigned int)(header[8] & 0x7F) << 7) |
        ((unsigned int)(header[9] & 0x7F));

    return 1;
}

static int add_album_art(
    const char *mp3_filename,
    const char *cover_filename)
{
    FILE *mp3;
    unsigned char *cover_data = NULL;
    size_t cover_size = 0;

    if (!read_binary_file(
            cover_filename,
            &cover_data,
            &cover_size))
    {
        printf(
            "Could not read album art: %s\n",
            cover_filename
        );

        return 0;
    }

    if (cover_size > 0x7FFFFF)
    {
        fprintf(
            stderr,
            "Album art is too large.\n"
        );

        free(cover_data);
        return 0;
    }

    mp3 = fopen(mp3_filename, "rb+");

    if (mp3 == NULL)
    {
        perror("fopen");
        free(cover_data);
        return 0;
    }

    unsigned int old_tag_size = 0;

    if (!find_id3v2_size(
            mp3,
            &old_tag_size))
    {
        fclose(mp3);
        free(cover_data);
        return 0;
    }

    if (old_tag_size == 0)
    {
        fclose(mp3);

        printf(
            "Skipping artwork for %s: "
            "MP3 does not have an ID3v2.3 tag.\n",
            mp3_filename
        );

        free(cover_data);
        return 0;
    }

    if (fseek(mp3, 10, SEEK_SET) != 0)
    {
        fclose(mp3);
        free(cover_data);
        return 0;
    }

    unsigned char *old_tag =
        malloc(old_tag_size);

    if (old_tag == NULL)
    {
        fclose(mp3);
        free(cover_data);
        return 0;
    }

    if (fread(
            old_tag,
            1,
            old_tag_size,
            mp3) != old_tag_size)
    {
        free(old_tag);
        fclose(mp3);
        free(cover_data);
        return 0;
    }

    size_t apic_size =
        1 +
        strlen("image/jpeg") + 1 +
        1 +
        1 +
        1 +
        1 +
        2 +
        cover_size;

    size_t apic_frame_size =
        10 + apic_size;

    size_t new_tag_size =
        (size_t)old_tag_size +
        apic_frame_size;

    if (new_tag_size > 0x0FFFFFFF)
    {
        fprintf(
            stderr,
            "ID3 tag would be too large.\n"
        );

        free(old_tag);
        fclose(mp3);
        free(cover_data);
        return 0;
    }

    unsigned char *new_tag =
        malloc(new_tag_size);

    if (new_tag == NULL)
    {
        free(old_tag);
        fclose(mp3);
        free(cover_data);
        return 0;
    }

    memcpy(
        new_tag,
        old_tag,
        old_tag_size
    );

    size_t position = old_tag_size;

    memcpy(
        &new_tag[position],
        "APIC",
        4
    );

    unsigned int frame_size =
        (unsigned int)apic_size;

    new_tag[position + 4] =
        (unsigned char)((frame_size >> 24) & 0xFF);

    new_tag[position + 5] =
        (unsigned char)((frame_size >> 16) & 0xFF);

    new_tag[position + 6] =
        (unsigned char)((frame_size >> 8) & 0xFF);

    new_tag[position + 7] =
        (unsigned char)(frame_size & 0xFF);

    new_tag[position + 8] = 0;
    new_tag[position + 9] = 0;

    position += 10;

    new_tag[position++] = 0;

    memcpy(
        &new_tag[position],
        "image/jpeg",
        strlen("image/jpeg")
    );

    position += strlen("image/jpeg");

    new_tag[position++] = '\0';

    new_tag[position++] = 3;

    new_tag[position++] = 0;

    new_tag[position++] = 0;

    new_tag[position++] = 0;

    new_tag[position++] = 0;

    new_tag[position++] = 0;

    new_tag[position++] = 0;

    memcpy(
        &new_tag[position],
        cover_data,
        cover_size
    );

    position += cover_size;

    if (fseek(mp3, 0, SEEK_SET) != 0)
    {
        free(new_tag);
        free(old_tag);
        fclose(mp3);
        free(cover_data);
        return 0;
    }

    unsigned char header[10];

    if (fread(header, 1, 10, mp3) != 10)
    {
        free(new_tag);
        free(old_tag);
        fclose(mp3);
        free(cover_data);
        return 0;
    }

    header[3] = 3;
    header[4] = 0;
    header[5] = 0;

    write_synchsafe(
        &header[6],
        (unsigned int)new_tag_size
    );

    if (fseek(mp3, 0, SEEK_SET) != 0)
    {
        free(new_tag);
        free(old_tag);
        fclose(mp3);
        free(cover_data);
        return 0;
    }

    if (fwrite(header, 1, 10, mp3) != 10)
    {
        free(new_tag);
        free(old_tag);
        fclose(mp3);
        free(cover_data);
        return 0;
    }

    if (fwrite(
            new_tag,
            1,
            new_tag_size,
            mp3) != new_tag_size)
    {
        free(new_tag);
        free(old_tag);
        fclose(mp3);
        free(cover_data);
        return 0;
    }

    free(new_tag);
    free(old_tag);

    fclose(mp3);
    free(cover_data);

    printf(
        "Album art embedded: %s\n",
        mp3_filename
    );

    return 1;
}

static int find_existing_folder_jpg(
    const char *album_path,
    char *cover_path,
    size_t cover_path_size)
{
    int length;

    length =
        snprintf(
            cover_path,
            cover_path_size,
            "%s/folder.jpg",
            album_path
        );

    if (length < 0 ||
        (size_t)length >= cover_path_size)
    {
        return 0;
    }

    struct stat st;

    if (stat(cover_path, &st) != 0)
    {
        return 0;
    }

    if (!S_ISREG(st.st_mode))
    {
        return 0;
    }

    return 1;
}

static int collect_music_files(
    const char *folder_path,
    struct music_file *files,
    int max_files)
{
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    dir = opendir(folder_path);

    if (dir == NULL)
    {
        perror("opendir");
        return 0;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        if (count >= max_files)
        {
            break;
        }

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        const char *ext =
            strrchr(entry->d_name, '.');

        if (ext == NULL ||
            strcasecmp(ext, ".mp3") != 0)
        {
            continue;
        }

        char full_path[PATH_MAX];

        int path_length =
            snprintf(
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

        memset(
            &files[count],
            0,
            sizeof(files[count])
        );

        snprintf(
            files[count].source_path,
            sizeof(files[count].source_path),
            "%s",
            full_path
        );

        snprintf(
            files[count].filename,
            sizeof(files[count].filename),
            "%s",
            entry->d_name
        );

        files[count].disc_number = 1;
        files[count].track_number = 0;

        char year[32] = "";
        char comment[64] = "";
        char genre[64] = "";

        int track = 0;
        int disc = 1;

        int found =
            read_id3v2_tags(
                full_path,
                files[count].artist,
                sizeof(files[count].artist),
                files[count].album,
                sizeof(files[count].album),
                files[count].title,
                sizeof(files[count].title),
                comment,
                sizeof(comment),
                year,
                sizeof(year),
                genre,
                sizeof(genre),
                &track,
                &disc
            );

        if (found)
        {
            files[count].track_number = track;
            files[count].disc_number = disc;
        }
        else
        {
            found =
                read_mp3_tags(
                    full_path,
                    files[count].artist,
                    sizeof(files[count].artist),
                    files[count].album,
                    sizeof(files[count].album),
                    files[count].title,
                    sizeof(files[count].title),
                    year,
                    sizeof(year),
                    comment,
                    sizeof(comment),
                    genre,
                    sizeof(genre)
                );
        }

        if (files[count].disc_number <= 0)
        {
            files[count].disc_number = 1;
        }

        if (files[count].track_number < 0)
        {
            files[count].track_number = 0;
        }

        if (files[count].artist[0] == '\0' ||
            files[count].album[0] == '\0')
        {
            printf(
                "Skipping %s: missing Artist or Album metadata.\n",
                entry->d_name
            );

            continue;
        }

        count++;
    }

    closedir(dir);

    return count;
}

static int organize_album(
    const char *base_path,
    struct music_file *files,
    int count)
{
    if (count <= 0)
    {
        return 0;
    }

    qsort(
        files,
        count,
        sizeof(struct music_file),
        compare_music_files
    );

    char artist[MAX_TEXT];
    char album[MAX_TEXT];

    snprintf(
        artist,
        sizeof(artist),
        "%s",
        files[0].artist
    );

    snprintf(
        album,
        sizeof(album),
        "%s",
        files[0].album
    );

    sanitize_filename(artist);
    sanitize_filename(album);

    char artist_path[PATH_MAX];
    char album_path[PATH_MAX];

    int length =
        snprintf(
            artist_path,
            sizeof(artist_path),
            "%s/%s",
            base_path,
            artist
        );

    if (length < 0 ||
        (size_t)length >= sizeof(artist_path))
    {
        fprintf(
            stderr,
            "Artist path too long.\n"
        );

        return 0;
    }

    create_directory(artist_path);

    length =
        snprintf(
            album_path,
            sizeof(album_path),
            "%s/%s",
            artist_path,
            album
        );

    if (length < 0 ||
        (size_t)length >= sizeof(album_path))
    {
        fprintf(
            stderr,
            "Album path too long.\n"
        );

        return 0;
    }

    create_directory(album_path);

    printf(
        "\nOrganizing: %s - %s\n",
        artist,
        album
    );

    printf(
        "Combining %d track(s) into one album.\n",
        count
    );

    int new_track_number = 1;

    for (int i = 0; i < count; i++)
    {
        char safe_title[MAX_TEXT];

        snprintf(
            safe_title,
            sizeof(safe_title),
            "%s",
            files[i].title
        );

        sanitize_filename(safe_title);

        char new_filename[PATH_MAX];

        const char *extension =
            strrchr(files[i].filename, '.');

        if (extension == NULL)
        {
            extension = ".mp3";
        }

        if (safe_title[0] != '\0')
        {
            snprintf(
                new_filename,
                sizeof(new_filename),
                "%02d - %s%s",
                new_track_number,
                safe_title,
                extension
            );
        }
        else
        {
            snprintf(
                new_filename,
                sizeof(new_filename),
                "%02d - %s%s",
                new_track_number,
                files[i].filename,
                extension
            );
        }

        char destination[PATH_MAX];

        if (!move_file(
                files[i].source_path,
                base_path,
                artist,
                album,
                new_filename,
                destination,
                sizeof(destination)))
        {
            continue;
        }

        printf(
            "Disc %d Track %d -> Track %d\n",
            files[i].disc_number,
            files[i].track_number,
            new_track_number
        );

        new_track_number++;
    }

    char cover_path[PATH_MAX];

    if (find_existing_folder_jpg(
            album_path,
            cover_path,
            sizeof(cover_path)))
    {
        printf(
            "Found existing album art: %s\n",
            cover_path
        );

        DIR *album_dir =
            opendir(album_path);

        if (album_dir != NULL)
        {
            struct dirent *entry;

            while ((entry =
                    readdir(album_dir)) != NULL)
            {
                const char *extension =
                    strrchr(entry->d_name, '.');

                if (extension == NULL ||
                    strcasecmp(extension, ".mp3") != 0)
                {
                    continue;
                }

                char mp3_path[PATH_MAX];

                length =
                    snprintf(
                        mp3_path,
                        sizeof(mp3_path),
                        "%s/%s",
                        album_path,
                        entry->d_name
                    );

                if (length < 0 ||
                    (size_t)length >= sizeof(mp3_path))
                {
                    continue;
                }

                add_album_art(
                    mp3_path,
                    cover_path
                );
            }

            closedir(album_dir);
        }
    }
    else
    {
        printf(
            "No existing folder.jpg found for this album.\n"
        );
    }

    return 1;
}

int main(void)
{
    const char *folder_path =
        "/home/kate/Music/";

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

    struct music_file files[MAX_TRACKS];

    int file_count =
        collect_music_files(
            folder_path,
            files,
            MAX_TRACKS
        );

    if (file_count == 0)
    {
        printf(
            "No usable MP3 files found.\n"
        );

        return 0;
    }

    int start = 0;

    while (start < file_count)
    {
        int end = start + 1;

        while (end < file_count)
        {
            if (strcasecmp(
                    files[start].artist,
                    files[end].artist) != 0 ||
                strcasecmp(
                    files[start].album,
                    files[end].album) != 0)
            {
                break;
            }

            end++;
        }

        organize_album(
            folder_path,
            &files[start],
            end - start
        );

        start = end;
    }

    printf(
        "\nMusic organization complete.\n"
    );

    return 0;
}
