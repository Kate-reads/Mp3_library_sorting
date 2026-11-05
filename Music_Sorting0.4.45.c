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

#define MUSIC_FOLDER "/home/kate/Music/"

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


/* =========================================================
   STRING HELPERS
   ========================================================= */

static void trim_string(char *string)
{
    size_t length;

    if (string == NULL)
        return;

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
        memmove(string, string + 1, strlen(string));
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
        return;

    length = source_size;

    if (length > destination_size - 1)
        length = destination_size - 1;

    memcpy(destination, source, length);

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
        destination_size <= 0)
        return;

    destination[0] = '\0';

    if (source == NULL || source_size < 2)
        return;

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

        if (character == 0)
            break;

        if (character < 128)
            destination[output++] = (char)character;
        else
            destination[output++] = '?';
    }

    destination[output] = '\0';

    trim_string(destination);
}


static int parse_first_number(const char *text)
{
    if (text == NULL)
        return 0;

    while (*text != '\0' &&
           !isdigit((unsigned char)*text))
    {
        text++;
    }

    if (*text == '\0')
        return 0;

    return atoi(text);
}


static void sanitize_filename(char *string)
{
    size_t i;

    if (string == NULL)
        return;

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


/* =========================================================
   ID3v1
   ========================================================= */

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
        return 0;

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
        return 0;

    copy_field(title, title_size, &tag[3], 30);
    copy_field(artist, artist_size, &tag[33], 30);
    copy_field(album, album_size, &tag[63], 30);
    copy_field(year, year_size, &tag[93], 4);
    copy_field(comment, comment_size, &tag[97], 30);

    if (genre != NULL && genre_size > 0)
    {
        snprintf(
            genre,
            genre_size,
            "%d",
            tag[127]
        );
    }

    return 1;
}


/* =========================================================
   ID3v2.3
   ========================================================= */

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
    FILE *file;
    unsigned char header[10];

    (void)comment;
    (void)comment_size;

    file = fopen(filename, "rb");

    if (file == NULL)
        return 0;

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
        ((unsigned int)(header[6] & 0x7F) << 21) |
        ((unsigned int)(header[7] & 0x7F) << 14) |
        ((unsigned int)(header[8] & 0x7F) << 7) |
        ((unsigned int)(header[9] & 0x7F));

    if (tag_size == 0)
    {
        fclose(file);
        return 1;
    }

    unsigned char *tag_data =
        malloc(tag_size);

    if (tag_data == NULL)
    {
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
            break;

        unsigned int frame_size =
            ((unsigned int)tag_data[position + 4] << 24) |
            ((unsigned int)tag_data[position + 5] << 16) |
            ((unsigned int)tag_data[position + 6] << 8) |
            ((unsigned int)tag_data[position + 7]);

        if (frame_size == 0)
        {
            position += 10;
            continue;
        }

        if (position + 10 + frame_size > tag_size)
            break;

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

                char text[MAX_TEXT];

                text[0] = '\0';

                if (encoding == 0)
                {
                    unsigned int length =
                        frame_size - 1;

                    if (length >= sizeof(text))
                        length = sizeof(text) - 1;

                    memcpy(
                        text,
                        &frame_data[1],
                        length
                    );

                    text[length] = '\0';

                    trim_string(text);
                }
                else if (encoding == 1)
                {
                    copy_utf16_text(
                        text,
                        sizeof(text),
                        &frame_data[1],
                        frame_size - 1
                    );
                }

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
                        parse_first_number(text);
                }
                else if (strcmp(frame_id, "TPOS") == 0)
                {
                    *disc_number =
                        parse_first_number(text);
                }
            }
        }

        position += 10 + frame_size;
    }

    free(tag_data);

    return 1;
}


/* =========================================================
   DIRECTORY
   ========================================================= */

static void create_directory(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
    {
        if (mkdir(path, 0755) != 0)
            perror("mkdir");
    }
}


/* =========================================================
   COLLECT MP3 FILES
   ========================================================= */

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
            break;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        const char *extension =
            strrchr(entry->d_name, '.');

        if (extension == NULL ||
            strcasecmp(extension, ".mp3") != 0)
            continue;

        char full_path[PATH_MAX];

        int length =
            snprintf(
                full_path,
                sizeof(full_path),
                "%s/%s",
                folder_path,
                entry->d_name
            );

        if (length < 0 ||
            (size_t)length >= sizeof(full_path))
            continue;

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
            files[count].disc_number = 1;

        if (files[count].track_number < 0)
            files[count].track_number = 0;

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


/* =========================================================
   SORT
   ========================================================= */

static int compare_music_files(
    const void *a,
    const void *b)
{
    const struct music_file *file_a = a;
    const struct music_file *file_b = b;

    int result;

    /*
     * FIRST:
     * Artist alphabetically
     */
    result =
        strcasecmp(
            file_a->artist,
            file_b->artist
        );

    if (result != 0)
        return result;

    /*
     * SECOND:
     * Album alphabetically
     */
    result =
        strcasecmp(
            file_a->album,
            file_b->album
        );

    if (result != 0)
        return result;

    /*
     * THIRD:
     * Disc number
     */
    if (file_a->disc_number !=
        file_b->disc_number)
    {
        return
            file_a->disc_number -
            file_b->disc_number;
    }

    /*
     * FOURTH:
     * Track number
     */
    if (file_a->track_number !=
        file_b->track_number)
    {
        return
            file_a->track_number -
            file_b->track_number;
    }

    return strcasecmp(
        file_a->filename,
        file_b->filename
    );
}


/* =========================================================
   FIND ARTWORK
   ========================================================= */

static int find_numbered_folder_jpg(
    const char *base_path,
    int album_number,
    char *cover_path,
    size_t cover_path_size)
{
    struct stat st;

    int length =
        snprintf(
            cover_path,
            cover_path_size,
            "%s/folder%d.jpg",
            base_path,
            album_number
        );

    if (length < 0 ||
        (size_t)length >= cover_path_size)
        return 0;

    if (stat(cover_path, &st) != 0)
        return 0;

    if (!S_ISREG(st.st_mode))
        return 0;

    return 1;
}


/* =========================================================
   ADD ALBUM ART
   ========================================================= */

static int add_album_art(
    const char *mp3_filename,
    const char *cover_filename)
{
    FILE *source;
    FILE *temp;

    unsigned char *cover_data = NULL;
    size_t cover_size = 0;

    /*
     * Read JPEG.
     */
    FILE *cover =
        fopen(cover_filename, "rb");

    if (cover == NULL)
    {
        perror("album artwork");
        return 0;
    }

    if (fseek(cover, 0, SEEK_END) != 0)
    {
        fclose(cover);
        return 0;
    }

    long cover_length =
        ftell(cover);

    if (cover_length <= 0)
    {
        fclose(cover);
        return 0;
    }

    rewind(cover);

    cover_size =
        (size_t)cover_length;

    cover_data =
        malloc(cover_size);

    if (cover_data == NULL)
    {
        fclose(cover);
        return 0;
    }

    if (fread(
            cover_data,
            1,
            cover_size,
            cover) != cover_size)
    {
        free(cover_data);
        fclose(cover);
        return 0;
    }

    fclose(cover);

    /*
     * Open MP3.
     */
    source =
        fopen(mp3_filename, "rb");

    if (source == NULL)
    {
        free(cover_data);
        return 0;
    }

    /*
     * Read ID3v2.3 header.
     */
    unsigned char header[10];

    if (fread(header, 1, 10, source) != 10)
    {
        fclose(source);
        free(cover_data);
        return 0;
    }

    if (memcmp(header, "ID3", 3) != 0 ||
        header[3] != 3)
    {
        printf(
            "Skipping artwork for %s: "
            "no ID3v2.3 tag.\n",
            mp3_filename
        );

        fclose(source);
        free(cover_data);
        return 0;
    }

    unsigned int old_tag_size =
        ((unsigned int)(header[6] & 0x7F) << 21) |
        ((unsigned int)(header[7] & 0x7F) << 14) |
        ((unsigned int)(header[8] & 0x7F) << 7) |
        ((unsigned int)(header[9] & 0x7F));

    /*
     * Read old tag.
     */
    unsigned char *old_tag =
        malloc(old_tag_size);

    if (old_tag == NULL)
    {
        fclose(source);
        free(cover_data);
        return 0;
    }

    if (fread(
            old_tag,
            1,
            old_tag_size,
            source) != old_tag_size)
    {
        free(old_tag);
        fclose(source);
        free(cover_data);
        return 0;
    }

    /*
     * Build APIC frame.
     */
    const char *mime =
        "image/jpeg";

    size_t mime_length =
        strlen(mime);

    size_t apic_size =
        1 +
        mime_length + 1 +
        1 +
        1 +
        cover_size;

    size_t apic_total =
        10 + apic_size;

    size_t new_tag_size =
        (size_t)old_tag_size +
        apic_total;

    if (new_tag_size > 0x0FFFFFFF)
    {
        free(old_tag);
        fclose(source);
        free(cover_data);
        return 0;
    }

    unsigned char *new_tag =
        malloc(new_tag_size);

    if (new_tag == NULL)
    {
        free(old_tag);
        fclose(source);
        free(cover_data);
        return 0;
    }

    /*
     * Copy old tag.
     */
    memcpy(
        new_tag,
        old_tag,
        old_tag_size
    );

    size_t position =
        old_tag_size;

    /*
     * APIC frame ID.
     */
    memcpy(
        &new_tag[position],
        "APIC",
        4
    );

    /*
     * Frame size.
     */
    new_tag[position + 4] =
        (unsigned char)((apic_size >> 24) & 0xFF);

    new_tag[position + 5] =
        (unsigned char)((apic_size >> 16) & 0xFF);

    new_tag[position + 6] =
        (unsigned char)((apic_size >> 8) & 0xFF);

    new_tag[position + 7] =
        (unsigned char)(apic_size & 0xFF);

    /*
     * Flags.
     */
    new_tag[position + 8] = 0;
    new_tag[position + 9] = 0;

    position += 10;

    /*
     * Encoding = ISO-8859-1.
     */
    new_tag[position++] = 0;

    /*
     * MIME.
     */
    memcpy(
        &new_tag[position],
        mime,
        mime_length
    );

    position += mime_length;

    new_tag[position++] = '\0';

    /*
     * Picture type 3 = front cover.
     */
    new_tag[position++] = 3;

    /*
     * Empty description.
     */
    new_tag[position++] = '\0';

    /*
     * JPEG.
     */
    memcpy(
        &new_tag[position],
        cover_data,
        cover_size
    );

    position += cover_size;

    /*
     * Prepare new header.
     */
    header[3] = 3;
    header[4] = 0;
    header[5] = 0;

    header[6] =
        (unsigned char)((new_tag_size >> 21) & 0x7F);

    header[7] =
        (unsigned char)((new_tag_size >> 14) & 0x7F);

    header[8] =
        (unsigned char)((new_tag_size >> 7) & 0x7F);

    header[9] =
        (unsigned char)(new_tag_size & 0x7F);

    /*
     * Temporary file.
     */
    char temp_filename[PATH_MAX];

    int length =
        snprintf(
            temp_filename,
            sizeof(temp_filename),
            "%s.artwork.tmp",
            mp3_filename
        );

    if (length < 0 ||
        (size_t)length >= sizeof(temp_filename))
    {
        free(new_tag);
        free(old_tag);
        fclose(source);
        free(cover_data);
        return 0;
    }

    temp =
        fopen(temp_filename, "wb");

    if (temp == NULL)
    {
        perror("temporary file");
        free(new_tag);
        free(old_tag);
        fclose(source);
        free(cover_data);
        return 0;
    }

    /*
     * Write new header.
     */
    if (fwrite(
            header,
            1,
            10,
            temp) != 10)
    {
        fclose(temp);
        fclose(source);
        unlink(temp_filename);
        free(new_tag);
        free(old_tag);
        free(cover_data);
        return 0;
    }

    /*
     * Write new ID3 tag.
     */
    if (fwrite(
            new_tag,
            1,
            new_tag_size,
            temp) != new_tag_size)
    {
        fclose(temp);
        fclose(source);
        unlink(temp_filename);
        free(new_tag);
        free(old_tag);
        free(cover_data);
        return 0;
    }

    /*
     * The source file is currently positioned
     * immediately after the old ID3 tag.
     *
     * Copy everything else unchanged.
     */
    unsigned char buffer[65536];

    size_t bytes_read;

    while ((bytes_read =
            fread(
                buffer,
                1,
                sizeof(buffer),
                source)) > 0)
    {
        if (fwrite(
                buffer,
                1,
                bytes_read,
                temp) != bytes_read)
        {
            fclose(temp);
            fclose(source);
            unlink(temp_filename);
            free(new_tag);
            free(old_tag);
            free(cover_data);
            return 0;
        }
    }

    if (ferror(source))
    {
        fclose(temp);
        fclose(source);
        unlink(temp_filename);
        free(new_tag);
        free(old_tag);
        free(cover_data);
        return 0;
    }

    fclose(source);

    if (fclose(temp) != 0)
    {
        unlink(temp_filename);
        free(new_tag);
        free(old_tag);
        free(cover_data);
        return 0;
    }

    /*
     * Replace original MP3.
     */
    if (rename(
            temp_filename,
            mp3_filename) != 0)
    {
        perror("rename");
        unlink(temp_filename);
        free(new_tag);
        free(old_tag);
        free(cover_data);
        return 0;
    }

    free(new_tag);
    free(old_tag);
    free(cover_data);

    printf(
        "Artwork embedded: %s\n",
        mp3_filename
    );

    return 1;
}


/* =========================================================
   MOVE FILE
   ========================================================= */

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
        return 0;

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
        return 0;

    create_directory(album_path);

    length =
        snprintf(
            destination,
            destination_size,
            "%s/%s",
            album_path,
            filename
        );

    if (length < 0 ||
        (size_t)length >= destination_size)
        return 0;

    /*
     * Don't overwrite existing files.
     */
    if (access(destination, F_OK) == 0)
    {
        fprintf(
            stderr,
            "Destination already exists, skipping:\n"
            "  %s\n",
            destination
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
        "Moved:\n"
        "  %s\n"
        "  -> %s\n",
        source_path,
        destination
    );

    return 1;
}


/* =========================================================
   ORGANIZE ONE ALBUM
   ========================================================= */

static int organize_album(
    const char *base_path,
    struct music_file *files,
    int count,
    int album_number)
{
    if (count <= 0)
        return 0;

    /*
     * Tracks belonging to this album are already
     * sorted, but sort again for safety.
     */
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

    printf(
        "\n========================================\n"
    );

    printf(
        "Album %d\n",
        album_number
    );

    printf(
        "Artist: %s\n",
        artist
    );

    printf(
        "Album:  %s\n",
        album
    );

    /*
     * ------------------------------------------------------
     * Artwork assignment happens AFTER alphabetical sorting.
     * ------------------------------------------------------
     */

    char cover_path[PATH_MAX];

    if (find_numbered_folder_jpg(
            base_path,
            album_number,
            cover_path,
            sizeof(cover_path)))
    {
        printf(
            "Artwork: %s\n",
            cover_path
        );
    }
    else
    {
        printf(
            "Artwork: NOT FOUND\n"
            "Expected: %s/folder%d.jpg\n",
            base_path,
            album_number
        );
    }

    /*
     * ------------------------------------------------------
     * Move tracks.
     * ------------------------------------------------------
     */

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
            extension = ".mp3";

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

        if (move_file(
                files[i].source_path,
                base_path,
                artist,
                album,
                new_filename,
                destination,
                sizeof(destination)))
        {
            printf(
                "Disc %d Track %d -> Track %d\n",
                files[i].disc_number,
                files[i].track_number,
                new_track_number
            );

            new_track_number++;
        }
    }

    /*
     * ------------------------------------------------------
     * Embed artwork into every MP3 in this album.
     * ------------------------------------------------------
     */

    if (!find_numbered_folder_jpg(
            base_path,
            album_number,
            cover_path,
            sizeof(cover_path)))
    {
        printf(
            "No artwork to embed for album %d.\n",
            album_number
        );

        return 1;
    }

    char album_path[PATH_MAX];

    int length =
        snprintf(
            album_path,
            sizeof(album_path),
            "%s/%s/%s",
            base_path,
            artist,
            album
        );

    if (length < 0 ||
        (size_t)length >= sizeof(album_path))
        return 0;

    DIR *album_dir =
        opendir(album_path);

    if (album_dir == NULL)
    {
        perror("opendir album");
        return 0;
    }

    struct dirent *entry;

    while ((entry = readdir(album_dir)) != NULL)
    {
        const char *extension =
            strrchr(entry->d_name, '.');

        if (extension == NULL ||
            strcasecmp(extension, ".mp3") != 0)
            continue;

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
            continue;

        add_album_art(
            mp3_path,
            cover_path
        );
    }

    closedir(album_dir);

    return 1;
}


/* =========================================================
   FILE COUNTER
   ========================================================= */

static int file_counter(const char *folder_path)
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
            continue;

        char full_path[PATH_MAX];

        int length =
            snprintf(
                full_path,
                sizeof(full_path),
                "%s/%s",
                folder_path,
                entry->d_name
            );

        if (length < 0 ||
            (size_t)length >= sizeof(full_path))
            continue;

        if (stat(full_path, &file_info) != 0)
            continue;

        if (!S_ISREG(file_info.st_mode))
            continue;

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


/* =========================================================
   MAIN
   ========================================================= */

int main(void)
{
    const char *folder_path =
        MUSIC_FOLDER;

    printf(
        "Music folder: %s\n",
        folder_path
    );

    int number_of_files =
        file_counter(folder_path);

    if (number_of_files < 0)
        return 1;

    printf(
        "Number of MP3 files: %d\n",
        number_of_files
    );

    if (number_of_files == 0)
    {
        printf("No MP3 files found.\n");
        return 0;
    }

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

    /*
     * ======================================================
     * IMPORTANT:
     *
     * SORT EVERYTHING BEFORE ASSIGNING ALBUM NUMBERS.
     *
     * This determines:
     *
     * folder1.jpg -> first album
     * folder2.jpg -> second album
     * folder3.jpg -> third album
     *
     * ======================================================
     */

    qsort(
        files,
        file_count,
        sizeof(struct music_file),
        compare_music_files
    );

    printf(
        "\nAlbums will be assigned artwork in this order:\n"
    );

    int preview_start = 0;
    int preview_album = 1;

    while (preview_start < file_count)
    {
        int preview_end =
            preview_start + 1;

        while (preview_end < file_count)
        {
            if (strcasecmp(
                    files[preview_start].artist,
                    files[preview_end].artist) != 0 ||
                strcasecmp(
                    files[preview_start].album,
                    files[preview_end].album) != 0)
            {
                break;
            }

            preview_end++;
        }

        printf(
            "  folder%d.jpg -> %s - %s\n",
            preview_album,
            files[preview_start].artist,
            files[preview_start].album
        );

        preview_album++;
        preview_start = preview_end;
    }

    /*
     * ======================================================
     * NOW ORGANIZE THE ALBUMS.
     * ======================================================
     */

    int start = 0;
    int album_number = 1;

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
            end - start,
            album_number
        );

        album_number++;
        start = end;
    }

    printf(
        "\n========================================\n"
    );

    printf(
        "Music organization complete.\n"
    );

    return 0;
}
