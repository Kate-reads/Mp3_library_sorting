#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>

#include <curl/curl.h>
#include <json-c/json.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_TRACKS 1000
#define MAX_TEXT 256

#define MUSIC_FOLDER "/home/kate/Music/"

/*
 * MusicBrainz requests require a meaningful application User-Agent.
 * Keep the application name and version current when changing releases.
 */
#define MUSICBRAINZ_USER_AGENT "MusicSorter/0.2.63 (MusicBrainz artwork lookup)"

struct music_file
{
    char source_path[PATH_MAX];
    char filename[PATH_MAX];

    char artist[MAX_TEXT];
    char album[MAX_TEXT];
    char title[MAX_TEXT];
    char year[MAX_TEXT];

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
        memmove(
            string,
            string + 1,
            strlen(string)
        );
    }
}

/*
 * ID3 text can contain UTF-8 whitespace/format characters that are not
 * handled by C's isspace().  Normalize the common ones so visually
 * identical metadata does not create separate album/artist groups.
 * This deliberately does not perform broad Unicode case folding or
 * transliteration.
 */
static void normalize_tag_text(char *string)
{
    char *read;
    char *write;

    if (string == NULL)
        return;

    read = string;
    write = string;

    while (*read != '\0')
    {
        if ((unsigned char)read[0] == 0xEF &&
            (unsigned char)read[1] == 0xBB &&
            (unsigned char)read[2] == 0xBF)
        {
            read += 3;
            continue;
        }

        /* UTF-8 non-breaking space -> ordinary space. */
        if ((unsigned char)read[0] == 0xC2 &&
            (unsigned char)read[1] == 0xA0)
        {
            *write++ = ' ';
            read += 2;
            continue;
        }

        /* UTF-8 zero-width space / word joiner. */
        if ((unsigned char)read[0] == 0xE2 &&
            (unsigned char)read[1] == 0x80 &&
            ((unsigned char)read[2] == 0x8B ||
             (unsigned char)read[2] == 0x8C ||
             (unsigned char)read[2] == 0x8D ||
             (unsigned char)read[2] == 0x8E))
        {
            read += 3;
            continue;
        }

        *write++ = *read++;
    }

    *write = '\0';
    trim_string(string);

    /* Collapse repeated ASCII whitespace so metadata variants group together. */
    read = string;
    write = string;
    int previous_space = 0;
    while (*read != '\0')
    {
        int current_space = isspace((unsigned char)*read) != 0;
        if (!(current_space && previous_space))
            *write++ = current_space ? ' ' : *read;
        previous_space = current_space;
        read++;
    }
    *write = '\0';
    trim_string(string);
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

    normalize_tag_text(destination);
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

    normalize_tag_text(destination);
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

static int read_mp3_tags(
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

    unsigned int id3_version = header[3];
    if (id3_version != 3 && id3_version != 4)
    {
        fclose(file);
        return 0;
    }

    /* Fail closed on tag-level layouts this parser does not implement. */
    if (header[5] != 0)
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

        unsigned int frame_size;
        if (id3_version == 4)
        {
            /* ID3v2.4 frame sizes are synchsafe integers. */
            if ((tag_data[position + 4] & 0x80) ||
                (tag_data[position + 5] & 0x80) ||
                (tag_data[position + 6] & 0x80) ||
                (tag_data[position + 7] & 0x80))
                break;
            frame_size =
                ((unsigned int)tag_data[position + 4] << 21) |
                ((unsigned int)tag_data[position + 5] << 14) |
                ((unsigned int)tag_data[position + 6] << 7) |
                (unsigned int)tag_data[position + 7];
        }
        else
        {
            frame_size =
                ((unsigned int)tag_data[position + 4] << 24) |
                ((unsigned int)tag_data[position + 5] << 16) |
                ((unsigned int)tag_data[position + 6] << 8) |
                ((unsigned int)tag_data[position + 7]);
        }

        if (frame_size == 0)
        {
            position += 10;
            continue;
        }

        /* Compressed, encrypted, grouped, or otherwise flagged frames are unsupported. */
        if (tag_data[position + 8] != 0 || tag_data[position + 9] != 0)
            break;

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

                    normalize_tag_text(text);
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
                else if (encoding == 3 && id3_version == 4)
                {
                    /* ID3v2.4 UTF-8 text; preserve bytes, bounded and terminated. */
                    unsigned int length = frame_size - 1;
                    if (length >= sizeof(text))
                        length = sizeof(text) - 1;
                    memcpy(text, &frame_data[1], length);
                    text[length] = '\0';
                    normalize_tag_text(text);
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

static int create_directory(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0)
    {
        if (!S_ISDIR(st.st_mode))
        {
            fprintf(stderr, "Path exists but is not a directory: %s\n", path);
            return 0;
        }
        return 1;
    }

    if (mkdir(path, 0755) == 0)
        return 1;

    if (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;

    perror(path);
    return 0;
}


/*
 * Find an existing directory under parent whose name matches wanted
 * case-insensitively.  This prevents artist/album folders such as
 * "Aurora" and "AURORA" from being created separately.
 */
static int find_existing_directory_ci(
    const char *parent,
    const char *wanted,
    char *result,
    size_t result_size)
{
    DIR *dir = opendir(parent);

    if (dir == NULL)
        return 0;

    struct dirent *entry;
    int found = 0;

    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '\0' ||
            strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        if (strcasecmp(entry->d_name, wanted) != 0)
            continue;

        char candidate[PATH_MAX];
        int length = snprintf(
            candidate,
            sizeof(candidate),
            "%s/%s",
            parent,
            entry->d_name
        );

        if (length < 0 || (size_t)length >= sizeof(candidate))
            continue;

        struct stat st;
        if (stat(candidate, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        length = snprintf(
            result,
            result_size,
            "%s",
            candidate
        );

        if (length >= 0 && (size_t)length < result_size)
            found = 1;

        break;
    }

    closedir(dir);
    return found;
}


/* =========================================================
   COLLECT MP3 FILES
   ========================================================= */

static int collect_music_files_recursive(
    const char *folder_path,
    struct music_file *files,
    int max_files,
    int *count)
{
    DIR *dir = opendir(folder_path);
    if (dir == NULL)
    {
        perror(folder_path);
        return 0;
    }

    struct dirent *entry;
    int success = 1;

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        char full_path[PATH_MAX];
        int length = snprintf(
            full_path, sizeof(full_path), "%s/%s",
            folder_path, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(full_path))
        {
            fprintf(stderr, "Skipping overlong path under %s\n", folder_path);
            continue;
        }

        struct stat info;
        /* lstat deliberately avoids following symlinks and directory loops. */
        if (lstat(full_path, &info) != 0)
        {
            perror(full_path);
            success = 0;
            continue;
        }

        if (S_ISDIR(info.st_mode))
        {
            if (!collect_music_files_recursive(
                    full_path, files, max_files, count))
                success = 0;
            continue;
        }

        if (!S_ISREG(info.st_mode))
            continue;

        const char *extension = strrchr(entry->d_name, '.');
        if (extension == NULL || strcasecmp(extension, ".mp3") != 0)
            continue;

        if (*count >= max_files)
        {
            fprintf(stderr, "MP3 count changed during scan; stopping at %d files.\n", max_files);
            success = 0;
            break;
        }

        struct music_file *file = &files[*count];
        memset(file, 0, sizeof(*file));
        snprintf(file->source_path, sizeof(file->source_path), "%s", full_path);
        snprintf(file->filename, sizeof(file->filename), "%s", entry->d_name);
        file->disc_number = 1;
        file->track_number = 0;

        char comment[64] = "";
        char genre[64] = "";
        int track = 0;
        int disc = 1;

        int found = read_id3v2_tags(
            full_path,
            file->artist, sizeof(file->artist),
            file->album, sizeof(file->album),
            file->title, sizeof(file->title),
            comment, sizeof(comment),
            file->year, sizeof(file->year),
            genre, sizeof(genre),
            &track, &disc);

        if (!found || file->artist[0] == '\0' || file->album[0] == '\0')
        {
            char v1_artist[MAX_TEXT] = "";
            char v1_album[MAX_TEXT] = "";
            char v1_title[MAX_TEXT] = "";
            char v1_year[MAX_TEXT] = "";
            char v1_comment[64] = "";
            char v1_genre[64] = "";

            if (read_mp3_tags(
                    full_path,
                    v1_artist, sizeof(v1_artist),
                    v1_album, sizeof(v1_album),
                    v1_title, sizeof(v1_title),
                    v1_year, sizeof(v1_year),
                    v1_comment, sizeof(v1_comment),
                    v1_genre, sizeof(v1_genre)))
            {
                if (file->artist[0] == '\0')
                    snprintf(file->artist, sizeof(file->artist), "%s", v1_artist);
                if (file->album[0] == '\0')
                    snprintf(file->album, sizeof(file->album), "%s", v1_album);
                if (file->title[0] == '\0')
                    snprintf(file->title, sizeof(file->title), "%s", v1_title);
                if (file->year[0] == '\0')
                    snprintf(file->year, sizeof(file->year), "%s", v1_year);
            }
        }

        file->track_number = track;
        file->disc_number = disc > 0 ? disc : 1;
        if (file->track_number < 0)
            file->track_number = 0;

        if (file->artist[0] == '\0' || file->album[0] == '\0')
        {
            printf("Skipping %s: missing Artist or Album metadata.\n", full_path);
            continue;
        }

        (*count)++;
    }

    closedir(dir);
    return success;
}

static int collect_music_files(
    const char *folder_path,
    struct music_file *files,
    int max_files)
{
    int count = 0;
    if (!collect_music_files_recursive(folder_path, files, max_files, &count))
        return count > 0 ? count : -1;
    return count;
}

/*
 * The two bonus tracks below are part of dodie's Build A Problem album.
 * Their source files may carry single-track/bonus-release album tags, so
 * normalize them before grouping.  This makes them land in the same folder
 * and gives them their requested positions in the album sequence.
 */
static void normalize_build_a_problem_bonus_tracks(
    struct music_file *files,
    int count)
{
    for (int i = 0; i < count; i++)
    {
        if (strcasecmp(files[i].artist, "dodie") != 0)
            continue;

        if (strcasecmp(files[i].title, "Guiltless") == 0)
        {
            snprintf(files[i].album, sizeof(files[i].album),
                     "%s", "Build A Problem");
            files[i].disc_number = 1;
            files[i].track_number = 13;
            printf("Special album mapping: Guiltless -> Build A Problem, track 13\n");
        }
        else if (strcasecmp(files[i].title, "Boys Like You") == 0)
        {
            snprintf(files[i].album, sizeof(files[i].album),
                     "%s", "Build A Problem");
            files[i].disc_number = 1;
            files[i].track_number = 14;
            printf("Special album mapping: Boys Like You -> Build A Problem, track 14\n");
        }
    }
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

    result =
        strcasecmp(
            file_a->artist,
            file_b->artist
        );

    if (result != 0)
        return result;

    result =
        strcasecmp(
            file_a->album,
            file_b->album
        );

    if (result != 0)
        return result;

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

    return strcasecmp(
        file_a->filename,
        file_b->filename
    );
}


/* =========================================================
   FIND ALBUM-LOCAL ARTWORK
   ========================================================= */

static int find_album_folder_jpg(
    const char *album_path,
    char *cover_path,
    size_t cover_path_size)
{
    struct stat st;

    int length = snprintf(
        cover_path,
        cover_path_size,
        "%s/folder.jpg",
        album_path
    );

    if (length < 0 || (size_t)length >= cover_path_size)
        return 0;

    if (stat(cover_path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;

    return 1;
}


/* =========================================================
   CURL MEMORY BUFFER
   ========================================================= */

struct memory_buffer
{
    unsigned char *data;
    size_t size;
};


static size_t curl_write_memory(
    void *contents,
    size_t size,
    size_t nmemb,
    void *userp)
{
    size_t total = size * nmemb;

    struct memory_buffer *memory =
        (struct memory_buffer *)userp;

    unsigned char *new_data =
        realloc(
            memory->data,
            memory->size + total + 1
        );

    if (new_data == NULL)
        return 0;

    memory->data = new_data;

    memcpy(
        &memory->data[memory->size],
        contents,
        total
    );

    memory->size += total;

    memory->data[memory->size] = '\0';

    return total;
}


/* =========================================================
   CURL DOWNLOAD TO FILE
   ========================================================= */

static int download_file(
    const char *url,
    const char *filename)
{
    FILE *file = fopen(filename, "wb");

    if (file == NULL)
    {
        perror("artwork output");
        return 0;
    }

    CURL *curl = curl_easy_init();

    if (curl == NULL)
    {
        fclose(file);
        unlink(filename);
        return 0;
    }

    char user_agent[256];
    snprintf(
        user_agent,
        sizeof(user_agent),
        "User-Agent: %s",
        MUSICBRAINZ_USER_AGENT
    );

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, user_agent);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);

    CURLcode result = curl_easy_perform(curl);

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    int close_result = fclose(file);

    if (result != CURLE_OK)
    {
        fprintf(
            stderr,
            "Artwork download failed: %s\n",
            curl_easy_strerror(result)
        );
        unlink(filename);
        return 0;
    }

    if (response_code < 200 || response_code >= 300)
    {
        fprintf(
            stderr,
            "Cover Art Archive HTTP status: %ld\n",
            response_code
        );
        unlink(filename);
        return 0;
    }

    if (close_result != 0)
    {
        perror("close downloaded artwork");
        unlink(filename);
        return 0;
    }

    return 1;
}

/* =========================================================
   URL ENCODING
   ========================================================= */

static int url_encode(
    const char *input,
    char *output,
    size_t output_size)
{
    CURL *curl;

    curl = curl_easy_init();

    if (curl == NULL)
        return 0;

    char *encoded =
        curl_easy_escape(
            curl,
            input,
            0
        );

    if (encoded == NULL)
    {
        curl_easy_cleanup(curl);
        return 0;
    }

    int length =
        snprintf(
            output,
            output_size,
            "%s",
            encoded
        );

    curl_free(encoded);
    curl_easy_cleanup(curl);

    if (length < 0 ||
        (size_t)length >= output_size)
        return 0;

    return 1;
}


/* =========================================================
   MUSICBRAINZ / COVER ART ARCHIVE SEARCH
   ========================================================= */

/*
 * Escape Lucene special characters before URL encoding a search term.
 * MusicBrainz uses Lucene syntax for its release search endpoint.
 */
static int lucene_escape_term(
    const char *input,
    char *output,
    size_t output_size)
{
    static const char special[] = "+-!(){}[]^\"~*?:\\/";

    size_t used = 0;

    if (input == NULL || output == NULL || output_size == 0)
        return 0;

    for (size_t i = 0; input[i] != '\0'; i++)
    {
        if (strchr(special, input[i]) != NULL)
        {
            if (used + 1 >= output_size)
                return 0;
            output[used++] = '\\';
        }

        if (used + 1 >= output_size)
            return 0;

        output[used++] = input[i];
    }

    output[used] = '\0';
    return 1;
}


/*
 * MusicBrainz artist-credit phrases can contain featured artists or
 * collaborations. Accept a case-insensitive containment match while still
 * requiring an exact album-title match before downloading artwork.
 */
static int artist_credit_matches(const char *credit, const char *wanted)
{
    if (credit == NULL || wanted == NULL || *credit == '\0' || *wanted == '\0')
        return 0;

    size_t credit_len = strlen(credit);
    size_t wanted_len = strlen(wanted);

    if (strcasecmp(credit, wanted) == 0)
        return 1;

    if (wanted_len <= credit_len)
    {
        for (size_t i = 0; i + wanted_len <= credit_len; i++)
        {
            if (strncasecmp(credit + i, wanted, wanted_len) == 0)
                return 1;
        }
    }

    if (credit_len <= wanted_len)
    {
        for (size_t i = 0; i + credit_len <= wanted_len; i++)
        {
            if (strncasecmp(wanted + i, credit, credit_len) == 0)
                return 1;
        }
    }

    return 0;
}


/*
 * MusicBrainz search responses can expose the combined artist credit as
 * "artist-credit-phrase" or as an "artist-credit" array. Support both
 * representations so a missing convenience field does not reject every
 * otherwise valid release.
 */
static int get_release_artist_credit(
    struct json_object *item,
    char *credit,
    size_t credit_size)
{
    struct json_object *phrase_obj = NULL;

    if (credit == NULL || credit_size == 0)
        return 0;

    credit[0] = '\0';

    if (json_object_object_get_ex(item, "artist-credit-phrase", &phrase_obj) &&
        phrase_obj != NULL &&
        json_object_is_type(phrase_obj, json_type_string))
    {
        const char *phrase = json_object_get_string(phrase_obj);
        if (phrase != NULL && phrase[0] != '\0')
        {
            snprintf(credit, credit_size, "%s", phrase);
            return 1;
        }
    }

    struct json_object *credit_array = NULL;
    if (!json_object_object_get_ex(item, "artist-credit", &credit_array) ||
        credit_array == NULL ||
        !json_object_is_type(credit_array, json_type_array))
        return 0;

    size_t used = 0;
    size_t count = json_object_array_length(credit_array);

    for (size_t i = 0; i < count; i++)
    {
        struct json_object *part = json_object_array_get_idx(credit_array, i);
        const char *piece = NULL;
        const char *joinphrase = "";

        if (part == NULL)
            continue;

        if (json_object_is_type(part, json_type_string))
        {
            /* Some representations store join phrases as string elements. */
            piece = json_object_get_string(part);
        }
        else if (json_object_is_type(part, json_type_object))
        {
            struct json_object *name_obj = NULL;
            struct json_object *artist_obj = NULL;
            struct json_object *artist_name_obj = NULL;
            struct json_object *join_obj = NULL;

            if (json_object_object_get_ex(part, "name", &name_obj) &&
                name_obj != NULL &&
                json_object_is_type(name_obj, json_type_string))
                piece = json_object_get_string(name_obj);

            if ((piece == NULL || piece[0] == '\0') &&
                json_object_object_get_ex(part, "artist", &artist_obj) &&
                artist_obj != NULL &&
                json_object_object_get_ex(artist_obj, "name", &artist_name_obj) &&
                artist_name_obj != NULL &&
                json_object_is_type(artist_name_obj, json_type_string))
                piece = json_object_get_string(artist_name_obj);

            if (json_object_object_get_ex(part, "joinphrase", &join_obj) &&
                join_obj != NULL &&
                json_object_is_type(join_obj, json_type_string))
            {
                const char *value = json_object_get_string(join_obj);
                if (value != NULL)
                    joinphrase = value;
            }
        }

        if (piece != NULL && piece[0] != '\0')
        {
            size_t piece_len = strlen(piece);
            if (piece_len > credit_size - used - 1)
                piece_len = credit_size - used - 1;
            memcpy(credit + used, piece, piece_len);
            used += piece_len;
            credit[used] = '\0';
        }

        size_t join_len = strlen(joinphrase);
        if (join_len > credit_size - used - 1)
            join_len = credit_size - used - 1;
        memcpy(credit + used, joinphrase, join_len);
        used += join_len;
        credit[used] = '\0';

        if (used >= credit_size - 1)
            break;
    }

    return credit[0] != '\0';
}


static int find_musicbrainz_artwork(
    const char *artist,
    const char *album,
    const char *year,
    const char *destination,
    size_t destination_size)
{
    if (destination == NULL || destination_size == 0)
        return 0;

    char escaped_album[MAX_TEXT * 2];
    char query[MAX_TEXT * 5];
    char encoded_query[MAX_TEXT * 10];
    char url[PATH_MAX * 2];

    if (!lucene_escape_term(
            album,
            escaped_album,
            sizeof(escaped_album)))
    {
        fprintf(stderr, "Could not prepare MusicBrainz search terms.\n");
        return 0;
    }

    /*
     * Search by album title first. MusicBrainz artist-credit phrases can
     * include featured artists or collaborations, so artist matching is
     * checked locally rather than requiring an exact Lucene artist phrase.
     */
    int query_length = snprintf(
        query,
        sizeof(query),
        "release:\"%s\"",
        escaped_album
    );

    if (query_length < 0 || (size_t)query_length >= sizeof(query))
    {
        fprintf(stderr, "MusicBrainz search query is too long.\n");
        return 0;
    }

    if (!url_encode(
            query,
            encoded_query,
            sizeof(encoded_query)))
        return 0;

    int url_length = snprintf(
        url,
        sizeof(url),
        "https://musicbrainz.org/ws/2/release/"
        "?query=%s&fmt=json&limit=100",
        encoded_query
    );

    if (url_length < 0 || (size_t)url_length >= sizeof(url))
    {
        fprintf(stderr, "MusicBrainz search URL is too long.\n");
        return 0;
    }

    printf(
        "Searching MusicBrainz:\n"
        "  Artist: %s\n"
        "  Album:  %s\n"
        "  Year:   %s\n",
        artist,
        album,
        year != NULL && year[0] != '\0' ? year : "unknown"
    );

    CURL *curl = curl_easy_init();

    if (curl == NULL)
        return 0;

    struct memory_buffer response = {NULL, 0};
    struct curl_slist *headers = NULL;

    char user_agent[256];
    snprintf(
        user_agent,
        sizeof(user_agent),
        "User-Agent: %s",
        MUSICBRAINZ_USER_AGENT
    );
    headers = curl_slist_append(headers, user_agent);
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_memory);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode result = curl_easy_perform(curl);

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /* MusicBrainz asks clients to keep web-service requests to one per second. */
    sleep(1);

    if (result != CURLE_OK)
    {
        fprintf(
            stderr,
            "MusicBrainz request failed: %s\n",
            curl_easy_strerror(result)
        );
        free(response.data);
        return 0;
    }

    if (response_code != 200 || response.data == NULL)
    {
        fprintf(stderr, "MusicBrainz HTTP status: %ld\n", response_code);
        free(response.data);
        return 0;
    }

    if (response.size > INT_MAX)
    {
        fprintf(stderr, "MusicBrainz response is too large to parse.\n");
        free(response.data);
        return 0;
    }

    struct json_tokener *tokener = json_tokener_new();

    if (tokener == NULL)
    {
        free(response.data);
        return 0;
    }

    struct json_object *root = json_tokener_parse_ex(
        tokener,
        (const char *)response.data,
        (int)response.size
    );

    enum json_tokener_error json_error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);
    free(response.data);

    if (json_error != json_tokener_success || root == NULL)
    {
        fprintf(stderr, "Could not parse MusicBrainz JSON.\n");
        if (root != NULL)
            json_object_put(root);
        return 0;
    }

    struct json_object *releases = NULL;

    if (!json_object_object_get_ex(root, "releases", &releases) ||
        releases == NULL ||
        !json_object_is_type(releases, json_type_array))
    {
        json_object_put(root);
        fprintf(stderr, "MusicBrainz returned no release list.\n");
        return 0;
    }

    size_t release_count = json_object_array_length(releases);
    int has_year = year != NULL && strlen(year) >= 4;
    int found = 0;
    int title_matches = 0;
    int artist_matches = 0;
    int cover_attempts = 0;

    printf("MusicBrainz returned %zu release result(s).\n", release_count);

    /*
     * Prefer an exact release-year match when the MP3 has a year.
     * If none has artwork, try other exact artist/title releases.
     */
    for (int pass = 0; pass < (has_year ? 2 : 1) && !found; pass++)
    {
        for (size_t i = 0; i < release_count; i++)
        {
            struct json_object *item =
                json_object_array_get_idx(releases, i);

            if (item == NULL)
                continue;

            struct json_object *id_obj = NULL;
            struct json_object *title_obj = NULL;
            struct json_object *date_obj = NULL;
            char release_artist[1024];

            if (!json_object_object_get_ex(item, "id", &id_obj) ||
                !json_object_object_get_ex(item, "title", &title_obj))
                continue;

            const char *release_id = json_object_get_string(id_obj);
            const char *release_title = json_object_get_string(title_obj);
            const char *release_date = "";

            if (!get_release_artist_credit(
                    item, release_artist, sizeof(release_artist)))
                release_artist[0] = '\0';

            if (json_object_object_get_ex(item, "date", &date_obj) &&
                date_obj != NULL)
            {
                const char *date_text = json_object_get_string(date_obj);
                if (date_text != NULL)
                    release_date = date_text;
            }

            if (release_id == NULL || release_title == NULL)
                continue;

            if (strcasecmp(release_title, album) != 0)
                continue;

            title_matches++;

            if (release_artist[0] == '\0' ||
                !artist_credit_matches(release_artist, artist))
                continue;

            artist_matches++;

            int year_matches =
                has_year &&
                release_date != NULL &&
                strncmp(release_date, year, 4) == 0;

            if (has_year && ((pass == 0 && !year_matches) ||
                             (pass == 1 && year_matches)))
                continue;

            char cover_url[256];
            int cover_url_length = snprintf(
                cover_url,
                sizeof(cover_url),
                "https://coverartarchive.org/release/%s/front-500",
                release_id
            );

            if (cover_url_length < 0 ||
                (size_t)cover_url_length >= sizeof(cover_url))
                continue;

            printf(
                "MusicBrainz match:\n"
                "  %s — %s\n"
                "  Release date: %s\n"
                "  Release ID: %s\n",
                release_artist,
                release_title,
                release_date[0] != '\0' ? release_date : "unknown",
                release_id
            );

            cover_attempts++;
            if (!download_file(cover_url, destination))
                continue;

            printf(
                "Cover Art Archive artwork downloaded:\n"
                "  %s\n"
                "Artwork source: MusicBrainz Cover Art Archive\n",
                destination
            );

            found = 1;
            break;
        }
    }

    json_object_put(root);

    if (!found)
    {
        printf("MusicBrainz match diagnostics:\n");
        printf("  Release results returned: %zu\n", release_count);
        printf("  Exact album-title matches: %d\n", title_matches);
        printf("  Artist-credit matches: %d\n", artist_matches);
        printf("  Cover Art Archive download attempts: %d\n", cover_attempts);

        if (release_count == 0)
            printf("  Reason: MusicBrainz returned no release results for this query.\n");
        else if (title_matches == 0)
            printf("  Reason: none of the returned releases had the exact album title.\n");
        else if (artist_matches == 0)
            printf("  Reason: album titles matched, but no artist credit matched the MP3 artist tag.\n");
        else if (cover_attempts > 0)
            printf("  Reason: matching releases were found, but no Cover Art Archive front cover could be downloaded.\n");

        printf("No suitable MusicBrainz / Cover Art Archive artwork found.\n");
    }

    return found;
}

/* =========================================================
   ALBUM ART
   ========================================================= */

static int add_album_art(
    const char *mp3_filename,
    const char *cover_filename)
{
    FILE *source = NULL;
    FILE *temp = NULL;
    FILE *cover = NULL;
    unsigned char *cover_data = NULL;
    unsigned char *old_tag = NULL;
    unsigned char *new_tag = NULL;
    size_t cover_size = 0;
    size_t old_tag_size = 0;
    size_t insert_position = 0;
    size_t new_tag_size = 0;
    char temp_filename[PATH_MAX];
    temp_filename[0] = '\0';
    int success = 0;
    int already_embedded = 0;

    cover = fopen(cover_filename, "rb");
    if (cover == NULL)
    {
        perror("album artwork");
        return 0;
    }

    if (fseek(cover, 0, SEEK_END) != 0)
        goto cleanup;

    long cover_length = ftell(cover);
    if (cover_length <= 0)
        goto cleanup;

    cover_size = (size_t)cover_length;
    if (cover_size > 0x0FFFFFFFU)
        goto cleanup;

    rewind(cover);

    cover_data = malloc(cover_size);
    if (cover_data == NULL)
        goto cleanup;

    if (fread(cover_data, 1, cover_size, cover) != cover_size)
        goto cleanup;

    /* This routine deliberately handles JPEG artwork only. */
    if (cover_size < 3 ||
        cover_data[0] != 0xFF ||
        cover_data[1] != 0xD8 ||
        cover_data[2] != 0xFF)
    {
        fprintf(
            stderr,
            "Skipping artwork for %s: artwork is not a JPEG.\n",
            mp3_filename
        );
        goto cleanup;
    }

    fclose(cover);
    cover = NULL;

    source = fopen(mp3_filename, "rb");
    if (source == NULL)
        goto cleanup;

    unsigned char header[10];
    if (fread(header, 1, sizeof(header), source) != sizeof(header))
        goto cleanup;

    if (memcmp(header, "ID3", 3) != 0 ||
        (header[3] != 3 && header[3] != 4))
    {
        fprintf(
            stderr,
            "Skipping artwork for %s: requires an ID3v2.3 or ID3v2.4 tag.\n",
            mp3_filename
        );
        goto cleanup;
    }

    /*
     * Refuse tag layouts that this writer does not implement.
     * v2.3: unsynchronisation and extended headers are unsupported.
     * v2.4: unsynchronisation, extended headers, and footers are unsupported.
     * The v2.4 experimental flag does not change frame layout.
     */
    unsigned int id3_version = header[3];
    unsigned char unsupported_tag_flags =
        (id3_version == 3) ? 0xC0 : 0xD0;

    if ((header[5] & unsupported_tag_flags) != 0)
    {
        fprintf(
            stderr,
            "Skipping artwork for %s: unsupported ID3v2.%u flags (0x%02X).\n",
            mp3_filename,
            id3_version,
            header[5]
        );
        goto cleanup;
    }

    old_tag_size =
        ((size_t)(header[6] & 0x7F) << 21) |
        ((size_t)(header[7] & 0x7F) << 14) |
        ((size_t)(header[8] & 0x7F) << 7) |
        ((size_t)(header[9] & 0x7F));

    if (old_tag_size == 0 || old_tag_size > 0x0FFFFFFFU)
        goto cleanup;

    old_tag = malloc(old_tag_size);
    if (old_tag == NULL)
        goto cleanup;

    if (fread(old_tag, 1, old_tag_size, source) != old_tag_size)
        goto cleanup;

    /*
     * Find the end of actual frames. ID3v2 padding is zero-filled and must
     * remain at the end of the tag. Appending APIC after the padding makes
     * many readers stop before the artwork, so insert before the padding.
     *
     * We also refuse malformed frame layouts rather than guessing where
     * the audio begins.
     */
    size_t position = 0;
    int found_apic = 0;

    while (position < old_tag_size)
    {
        size_t remaining = old_tag_size - position;

        if (remaining < 10)
        {
            for (size_t i = position; i < old_tag_size; i++)
            {
                if (old_tag[i] != 0)
                {
                    fprintf(
                        stderr,
                        "Skipping artwork for %s: malformed ID3 padding.\n",
                        mp3_filename
                    );
                    goto cleanup;
                }
            }
            break;
        }

        int all_zero = 1;
        for (size_t i = 0; i < 10; i++)
        {
            if (old_tag[position + i] != 0)
            {
                all_zero = 0;
                break;
            }
        }

        if (all_zero)
            break;

        int valid_frame_id = 1;
        for (size_t id = 0; id < 4; id++)
        {
            unsigned char c = old_tag[position + id];
            if (!((c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9')))
            {
                valid_frame_id = 0;
                break;
            }
        }

        if (!valid_frame_id)
        {
            fprintf(
                stderr,
                "Skipping artwork for %s: malformed ID3 frame header.\n",
                mp3_filename
            );
            goto cleanup;
        }

        unsigned int frame_size;

        if (id3_version == 4)
        {
            /* ID3v2.4 frame sizes are 28-bit synchsafe integers. */
            if ((old_tag[position + 4] & 0x80) != 0 ||
                (old_tag[position + 5] & 0x80) != 0 ||
                (old_tag[position + 6] & 0x80) != 0 ||
                (old_tag[position + 7] & 0x80) != 0)
            {
                fprintf(
                    stderr,
                    "Skipping artwork for %s: invalid ID3v2.4 frame size.\n",
                    mp3_filename
                );
                goto cleanup;
            }

            frame_size =
                ((unsigned int)old_tag[position + 4] << 21) |
                ((unsigned int)old_tag[position + 5] << 14) |
                ((unsigned int)old_tag[position + 6] << 7) |
                (unsigned int)old_tag[position + 7];
        }
        else
        {
            /* ID3v2.3 frame sizes use ordinary big-endian integers. */
            frame_size =
                ((unsigned int)old_tag[position + 4] << 24) |
                ((unsigned int)old_tag[position + 5] << 16) |
                ((unsigned int)old_tag[position + 6] << 8) |
                (unsigned int)old_tag[position + 7];
        }

        if ((size_t)frame_size > remaining - 10)
        {
            fprintf(
                stderr,
                "Skipping artwork for %s: ID3 frame exceeds tag boundary.\n",
                mp3_filename
            );
            goto cleanup;
        }

        if (memcmp(&old_tag[position], "APIC", 4) == 0)
            found_apic = 1;

        position += 10 + (size_t)frame_size;
    }

    insert_position = position;

    if (found_apic)
    {
        printf(
            "Replacing existing embedded artwork with album folder art: %s\n",
            mp3_filename
        );
    }

    const char *mime = "image/jpeg";
    size_t mime_length = strlen(mime);

    if (cover_size > SIZE_MAX - (1 + mime_length + 1 + 1 + 1))
        goto cleanup;

    size_t apic_size =
        1 +                 /* text encoding */
        mime_length + 1 +  /* MIME type */
        1 +                 /* picture type */
        1 +                 /* empty description terminator */
        cover_size;         /* picture data */

    if (apic_size > UINT32_MAX ||
        apic_size > SIZE_MAX - 10)
        goto cleanup;

    size_t apic_total = 10 + apic_size;

    /* Copy every existing frame except APIC, then append the chosen cover. */
    size_t compacted_size = 0;
    size_t scan = 0;
    while (scan < insert_position)
    {
        unsigned int frame_size;
        if (id3_version == 4)
        {
            frame_size =
                ((unsigned int)old_tag[scan + 4] << 21) |
                ((unsigned int)old_tag[scan + 5] << 14) |
                ((unsigned int)old_tag[scan + 6] << 7) |
                (unsigned int)old_tag[scan + 7];
        }
        else
        {
            frame_size =
                ((unsigned int)old_tag[scan + 4] << 24) |
                ((unsigned int)old_tag[scan + 5] << 16) |
                ((unsigned int)old_tag[scan + 6] << 8) |
                (unsigned int)old_tag[scan + 7];
        }
        size_t frame_total = 10 + (size_t)frame_size;
        if (memcmp(&old_tag[scan], "APIC", 4) != 0)
            compacted_size += frame_total;
        scan += frame_total;
    }

    size_t padding_size = old_tag_size - insert_position;
    if (compacted_size > SIZE_MAX - apic_total ||
        compacted_size + apic_total > SIZE_MAX - padding_size)
        goto cleanup;
    new_tag_size = compacted_size + apic_total + padding_size;

    if (new_tag_size > 0x0FFFFFFFU)
        goto cleanup;

    new_tag = malloc(new_tag_size);
    if (new_tag == NULL)
        goto cleanup;

    size_t output_position = 0;
    scan = 0;
    while (scan < insert_position)
    {
        unsigned int frame_size;
        if (id3_version == 4)
        {
            frame_size =
                ((unsigned int)old_tag[scan + 4] << 21) |
                ((unsigned int)old_tag[scan + 5] << 14) |
                ((unsigned int)old_tag[scan + 6] << 7) |
                (unsigned int)old_tag[scan + 7];
        }
        else
        {
            frame_size =
                ((unsigned int)old_tag[scan + 4] << 24) |
                ((unsigned int)old_tag[scan + 5] << 16) |
                ((unsigned int)old_tag[scan + 6] << 8) |
                (unsigned int)old_tag[scan + 7];
        }
        size_t frame_total = 10 + (size_t)frame_size;
        if (memcmp(&old_tag[scan], "APIC", 4) != 0)
        {
            memcpy(&new_tag[output_position], &old_tag[scan], frame_total);
            output_position += frame_total;
        }
        scan += frame_total;
    }

    position = output_position;
    memcpy(&new_tag[position], "APIC", 4);
    if (id3_version == 4)
    {
        /* ID3v2.4 frame sizes must be written as synchsafe integers. */
        if (apic_size > 0x0FFFFFFFU)
            goto cleanup;

        new_tag[position + 4] = (unsigned char)((apic_size >> 21) & 0x7F);
        new_tag[position + 5] = (unsigned char)((apic_size >> 14) & 0x7F);
        new_tag[position + 6] = (unsigned char)((apic_size >> 7) & 0x7F);
        new_tag[position + 7] = (unsigned char)(apic_size & 0x7F);
    }
    else
    {
        /* ID3v2.3 frame sizes use ordinary big-endian integers. */
        new_tag[position + 4] = (unsigned char)((apic_size >> 24) & 0xFF);
        new_tag[position + 5] = (unsigned char)((apic_size >> 16) & 0xFF);
        new_tag[position + 6] = (unsigned char)((apic_size >> 8) & 0xFF);
        new_tag[position + 7] = (unsigned char)(apic_size & 0xFF);
    }
    new_tag[position + 8] = 0;
    new_tag[position + 9] = 0;
    position += 10;

    new_tag[position++] = 0; /* ISO-8859-1 */
    memcpy(&new_tag[position], mime, mime_length);
    position += mime_length;
    new_tag[position++] = '\0';
    new_tag[position++] = 3; /* front cover */
    new_tag[position++] = '\0'; /* empty description */

    memcpy(&new_tag[position], cover_data, cover_size);
    position += cover_size;

    memcpy(
        &new_tag[position],
        &old_tag[insert_position],
        padding_size
    );

    unsigned char new_header[10];
    memcpy(new_header, header, sizeof(new_header));
    new_header[6] = (unsigned char)((new_tag_size >> 21) & 0x7F);
    new_header[7] = (unsigned char)((new_tag_size >> 14) & 0x7F);
    new_header[8] = (unsigned char)((new_tag_size >> 7) & 0x7F);
    new_header[9] = (unsigned char)(new_tag_size & 0x7F);

    int length = snprintf(
        temp_filename,
        sizeof(temp_filename),
        "%s.artwork.tmp",
        mp3_filename
    );

    if (length < 0 || (size_t)length >= sizeof(temp_filename))
        goto cleanup;

    /* Never reuse a stale temporary file. */
    if (unlink(temp_filename) != 0 && access(temp_filename, F_OK) == 0)
    {
        perror("remove stale artwork temporary file");
        goto cleanup;
    }

    temp = fopen(temp_filename, "wb");
    if (temp == NULL)
    {
        perror("temporary file");
        goto cleanup;
    }

    if (fwrite(new_header, 1, sizeof(new_header), temp) != sizeof(new_header) ||
        fwrite(new_tag, 1, new_tag_size, temp) != new_tag_size)
    {
        fprintf(stderr, "Failed writing temporary MP3: %s\n", mp3_filename);
        goto cleanup;
    }

    unsigned char buffer[65536];
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), source)) > 0)
    {
        if (fwrite(buffer, 1, bytes_read, temp) != bytes_read)
        {
            fprintf(stderr, "Failed copying audio data: %s\n", mp3_filename);
            goto cleanup;
        }
    }

    if (ferror(source))
    {
        fprintf(stderr, "Failed reading audio data: %s\n", mp3_filename);
        goto cleanup;
    }

    /* Flush the complete replacement before it can replace the original. */
    if (fflush(temp) != 0)
    {
        perror("fflush artwork temporary file");
        goto cleanup;
    }

    if (fsync(fileno(temp)) != 0)
    {
        perror("fsync artwork temporary file");
        goto cleanup;
    }

    if (fclose(temp) != 0)
    {
        temp = NULL;
        perror("close artwork temporary file");
        goto cleanup;
    }
    temp = NULL;

    fclose(source);
    source = NULL;

    /* Same-directory rename is atomic on the POSIX filesystems this program targets. */
    if (rename(temp_filename, mp3_filename) != 0)
    {
        perror("rename artwork temporary file");
        goto cleanup;
    }

    success = 1;

cleanup:
    if (temp != NULL)
        fclose(temp);

    if (source != NULL)
        fclose(source);

    if (cover != NULL)
        fclose(cover);

    if (!success && temp_filename[0] != '\0')
        unlink(temp_filename);

    free(new_tag);
    free(old_tag);
    free(cover_data);

    if (success && !already_embedded)
    {
        printf(
            "Artwork embedded successfully: %s\n",
            mp3_filename
        );
    }

    return success;
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
    int length;

    /* Reuse an existing artist directory regardless of tag capitalization. */
    if (!find_existing_directory_ci(
            base_path,
            artist,
            artist_path,
            sizeof(artist_path)))
    {
        length =
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

        if (!create_directory(artist_path))
            return 0;
    }

    /* Reuse an existing album directory regardless of tag capitalization. */
    if (!find_existing_directory_ci(
            artist_path,
            album,
            album_path,
            sizeof(album_path)))
    {
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

        if (!create_directory(album_path))
            return 0;
    }

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
   ALBUM TRACK NUMBERING AND DUPLICATE CHECK
   ========================================================= */

/*
 * The input list must be sorted by disc, ID3 track number, then filename.
 * Output filenames use one continuous sequence across the whole album:
 * disc 1 tracks first, followed by disc 2, disc 3, and so on.
 *
 * Duplicate ID3 track numbers are reported per disc. When two files have
 * the same disc and track tag, compare_music_files() places them in
 * filename order; the continuous output sequence then gives each a unique
 * filename number. The program cannot infer the intended order of two
 * genuinely ambiguous duplicate-tagged files, so the warning is important.
 */
static void assign_album_track_numbers(
    const struct music_file *files,
    int count,
    int *album_track_numbers)
{
    int used[MAX_TRACKS + 1] = {0};

    /* Reserve the explicitly requested Build A Problem bonus-track slots. */
    for (int i = 0; i < count; i++)
    {
        if (strcasecmp(files[i].artist, "dodie") == 0 &&
            strcasecmp(files[i].album, "Build A Problem") == 0)
        {
            if (strcasecmp(files[i].title, "Guiltless") == 0)
            {
                album_track_numbers[i] = 13;
                used[13] = 1;
            }
            else if (strcasecmp(files[i].title, "Boys Like You") == 0)
            {
                album_track_numbers[i] = 14;
                used[14] = 1;
            }
            else
            {
                album_track_numbers[i] = 0;
            }
        }
        else
        {
            album_track_numbers[i] = 0;
        }
    }

    /* Give every other track the next free album-wide number. */
    int next_number = 1;
    for (int i = 0; i < count; i++)
    {
        if (album_track_numbers[i] != 0)
            continue;

        while (next_number <= MAX_TRACKS && used[next_number])
            next_number++;

        if (next_number > MAX_TRACKS)
        {
            album_track_numbers[i] = i + 1;
            continue;
        }

        album_track_numbers[i] = next_number;
        used[next_number] = 1;
        next_number++;
    }

    /* Report duplicate ID3 track tags within each disc. */
    for (int i = 0; i < count; i++)
    {
        if (files[i].track_number <= 0)
            continue;

        int disc = files[i].disc_number > 0 ? files[i].disc_number : 1;

        for (int j = 0; j < i; j++)
        {
            int earlier_disc = files[j].disc_number > 0
                                   ? files[j].disc_number : 1;

            if (earlier_disc == disc &&
                files[j].track_number == files[i].track_number)
            {
                printf(
                    "WARNING: duplicate ID3 track number %d on disc %d\n"
                    "         %s\n"
                    "         %s\n"
                    "         Using filename order to break the tie;\n"
                    "         output numbers will remain unique unless a\n"
                    "         specifically assigned bonus-track number applies.\n",
                    files[i].track_number,
                    disc,
                    files[j].filename,
                    files[i].filename
                );
                break;
            }
        }
    }
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

    qsort(
        files,
        count,
        sizeof(struct music_file),
        compare_music_files
    );

    int album_track_numbers[MAX_TRACKS];
    if (count > MAX_TRACKS)
    {
        fprintf(stderr, "Too many tracks in album to number safely.\n");
        return 0;
    }
    assign_album_track_numbers(files, count, album_track_numbers);

    char artist[MAX_TEXT];
    char album[MAX_TEXT];
    char year[MAX_TEXT];

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

    snprintf(
        year,
        sizeof(year),
        "%s",
        files[0].year
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

    printf(
        "Year:   %s\n",
        year[0] != '\0' ? year : "unknown"
    );


    /*
     * ======================================================
     * MOVE TRACKS
     * ======================================================
     */

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

        /* Use the album-wide number, so disc 2 continues after disc 1. */
        int filename_track_number = album_track_numbers[i];

        int required;

        if (safe_title[0] != '\0')
        {
            required = snprintf(
                new_filename,
                sizeof(new_filename),
                "%02d - %s%s",
                filename_track_number,
                safe_title,
                extension
            );
        }
        else
        {
            /* Keep the existing filename when no title tag is available. */
            required = snprintf(
                new_filename,
                sizeof(new_filename),
                "%s",
                files[i].filename
            );
        }

        if (required < 0 || (size_t)required >= sizeof(new_filename))
        {
            fprintf(
                stderr,
                "Skipping filename that is too long: %s\n",
                files[i].filename
            );
            continue;
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
                "Disc %d Track %d -> Album Track %d\n",
                files[i].disc_number,
                files[i].track_number,
                filename_track_number
            );
        }
    }

    /*
     * ======================================================
     * Construct album path.
     * ======================================================
     */

    char artist_path[PATH_MAX];
    char album_path[PATH_MAX];

    /* Resolve the actual on-disk spelling; tags may differ in case. */
    if (!find_existing_directory_ci(
            base_path, artist, artist_path, sizeof(artist_path)) ||
        !find_existing_directory_ci(
            artist_path, album, album_path, sizeof(album_path)))
    {
        fprintf(stderr,
                "Could not resolve destination album directory for %s - %s\n",
                artist, album);
        return 0;
    }


    int length;

    /*
     * Prefer a folder.jpg already inside this album's directory.
     * MusicBrainz / Cover Art Archive is used only when it is absent.
     */
    char cover_path[PATH_MAX];
    int have_cover = 0;
    int local_cover = 0;
    cover_path[0] = '\0';

    if (find_album_folder_jpg(album_path, cover_path, sizeof(cover_path)))
    {
        have_cover = 1;
        local_cover = 1;
        printf("Artwork: using album-local folder.jpg\n  %s\n", cover_path);
    }
    else
    {
        printf("Artwork: album-local folder.jpg not found.\n");
    }

    /*
     * ======================================================
     * MUSICBRAINZ / COVER ART ARCHIVE FALLBACK
     *
     * Do this only when the album directory has no local
     * folder.jpg.
     * ======================================================
     */

    if (!have_cover)
    {
        char downloaded_cover[PATH_MAX];

        length =
            snprintf(
                downloaded_cover,
                sizeof(downloaded_cover),
                "%s/.musicbrainz-cover.jpg",
                album_path
            );

        if (length >= 0 &&
            (size_t)length < sizeof(downloaded_cover))
        {
            if (find_musicbrainz_artwork(
                    files[0].artist,
                    files[0].album,
                    files[0].year,
                    downloaded_cover,
                    sizeof(downloaded_cover)))
            {
                snprintf(
                    cover_path,
                    sizeof(cover_path),
                    "%s",
                    downloaded_cover
                );

                have_cover = 1;

                printf(
                    "Artwork source: MusicBrainz Cover Art Archive\n"
                );
            }
        }
    }


    /*
     * ======================================================
     * EMBED ARTWORK
     * ======================================================
     */

    if (have_cover)
    {
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


        /*
         * ==================================================
         * LOCAL ARTWORK
         *
         * Keep the album-local folder.jpg in place.
         * ==================================================
         */

        if (local_cover)
        {
            printf("Local folder.jpg left in place.\n");
        }
        else
        {
            /*
             * Cover Art Archive image was downloaded directly
             * as .musicbrainz-cover.jpg.
             *
             * Rename it to folder.jpg.
             */

            char final_cover[PATH_MAX];

            length =
                snprintf(
                    final_cover,
                    sizeof(final_cover),
                    "%s/folder.jpg",
                    album_path
                );

            if (length >= 0 &&
                (size_t)length < sizeof(final_cover))
            {
                if (access(
                        final_cover,
                        F_OK) != 0)
                {
                    if (rename(
                            cover_path,
                            final_cover) == 0)
                    {
                        printf(
                            "MusicBrainz artwork saved:\n"
                            "  %s\n",
                            final_cover
                        );
                    }
                    else
                    {
                        perror(
                            "rename MusicBrainz artwork"
                        );
                    }
                }
            }
        }
    }
    else
    {
        printf(
            "No artwork found for this album.\n"
        );
    }

    return 1;
}


/* =========================================================
   FILE COUNTER
   ========================================================= */

static int file_counter_recursive(const char *folder_path, int *total)
{
    DIR *folder = opendir(folder_path);
    if (folder == NULL)
    {
        perror(folder_path);
        return 0;
    }

    struct dirent *entry;
    int success = 1;

    while ((entry = readdir(folder)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        char full_path[PATH_MAX];
        int length = snprintf(
            full_path, sizeof(full_path), "%s/%s",
            folder_path, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(full_path))
        {
            fprintf(stderr, "Skipping overlong path under %s\n", folder_path);
            continue;
        }

        struct stat info;
        if (lstat(full_path, &info) != 0)
        {
            perror(full_path);
            success = 0;
            continue;
        }

        if (S_ISDIR(info.st_mode))
        {
            if (!file_counter_recursive(full_path, total))
                success = 0;
        }
        else if (S_ISREG(info.st_mode))
        {
            const char *extension = strrchr(entry->d_name, '.');
            if (extension != NULL && strcasecmp(extension, ".mp3") == 0)
                (*total)++;
        }
    }

    closedir(folder);
    return success;
}

static int file_counter(const char *folder_path)
{
    int total = 0;
    if (!file_counter_recursive(folder_path, &total))
        return -1;
    return total;
}

/* =========================================================
   DRY-RUN DESTINATION PREVIEW
   ========================================================= */

static void preview_album_tracks(
    const char *base_path,
    struct music_file *files,
    int count)
{
    if (count <= 0)
        return;

    qsort(files, count, sizeof(struct music_file), compare_music_files);
    if (count > MAX_TRACKS)
    {
        printf("  SKIP: too many tracks in album to preview safely\n");
        return;
    }
    int album_track_numbers[MAX_TRACKS];
    assign_album_track_numbers(files, count, album_track_numbers);

    char artist[MAX_TEXT];
    char album[MAX_TEXT];
    char artist_path[PATH_MAX];
    char album_path[PATH_MAX];

    snprintf(artist, sizeof(artist), "%s", files[0].artist);
    snprintf(album, sizeof(album), "%s", files[0].album);
    sanitize_filename(artist);
    sanitize_filename(album);

    if (!find_existing_directory_ci(
            base_path, artist, artist_path, sizeof(artist_path)))
    {
        int path_length = snprintf(artist_path, sizeof(artist_path),
                                   "%s/%s", base_path, artist);
        if (path_length < 0 || (size_t)path_length >= sizeof(artist_path))
        {
            printf("  SKIP: artist destination path is too long\n");
            return;
        }
    }

    if (!find_existing_directory_ci(
            artist_path, album, album_path, sizeof(album_path)))
    {
        int path_length = snprintf(album_path, sizeof(album_path),
                                   "%s/%s", artist_path, album);
        if (path_length < 0 || (size_t)path_length >= sizeof(album_path))
        {
            printf("  SKIP: album destination path is too long\n");
            return;
        }
    }

    for (int i = 0; i < count; i++)
    {
        char safe_title[MAX_TEXT];
        char new_filename[PATH_MAX];
        char destination[PATH_MAX];

        snprintf(safe_title, sizeof(safe_title), "%s", files[i].title);
        sanitize_filename(safe_title);

        const char *extension = strrchr(files[i].filename, '.');
        if (extension == NULL)
            extension = ".mp3";

        int filename_track_number = album_track_numbers[i];

        int needed;
        if (safe_title[0] != '\0')
        {
            needed = snprintf(new_filename, sizeof(new_filename),
                              "%02d - %s%s", filename_track_number,
                              safe_title, extension);
        }
        else
        {
            needed = snprintf(new_filename, sizeof(new_filename), "%s",
                              files[i].filename);
        }

        if (needed < 0 || (size_t)needed >= sizeof(new_filename))
        {
            printf("  SKIP: filename too long: %s\n", files[i].source_path);
            continue;
        }

        needed = snprintf(destination, sizeof(destination), "%s/%s",
                          album_path, new_filename);
        if (needed < 0 || (size_t)needed >= sizeof(destination))
        {
            printf("  SKIP: destination path too long: %s\n", files[i].source_path);
            continue;
        }

        printf("\n  Source:      %s\n", files[i].source_path);
        printf("  Disc/track:  %d/%d (ID3 metadata)\n",
               files[i].disc_number, files[i].track_number);
        printf("  Album track: %d (continuous numbering)\n",
               filename_track_number);
        printf("  New name:    %s\n", new_filename);
        printf("  Destination: %s\n", destination);

        if (strcmp(files[i].source_path, destination) == 0)
        {
            printf("  Status:      already in the correct location\n");
        }
        else if (access(destination, F_OK) == 0)
        {
            printf("  Status:      COLLISION - destination exists; will not overwrite\n");
        }
        else
        {
            printf("  Status:      would move (dry run only)\n");
        }
    }
}


/* =========================================================
   MAIN
   ========================================================= */

int main(void)
{
    const char *folder_override = getenv("MUSIC_FOLDER");
    const char *folder_path =
        (folder_override != NULL && folder_override[0] != '\0')
            ? folder_override : MUSIC_FOLDER;
    const char *dry_run_env = getenv("MUSIC_SORT_DRY_RUN");
    int dry_run = dry_run_env != NULL &&
                  (strcmp(dry_run_env, "1") == 0 ||
                   strcasecmp(dry_run_env, "true") == 0 ||
                   strcasecmp(dry_run_env, "yes") == 0);

    printf(
        "Music folder: %s\n",
        folder_path
    );
    if (dry_run)
        printf("DRY RUN: no files will be moved and no artwork will be downloaded or embedded.\n");

    printf(
        "Artwork database: MusicBrainz / Cover Art Archive\n"
        "Discogs integration: removed\n"
        "Existing embedded artwork will be preserved.\n"
    );

    if (curl_global_init(
            CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        fprintf(
            stderr,
            "Could not initialize libcurl.\n"
        );

        return 1;
    }

    int number_of_files =
        file_counter(folder_path);

    if (number_of_files < 0)
    {
        curl_global_cleanup();
        return 1;
    }

    printf(
        "Number of MP3 files: %d\n",
        number_of_files
    );

if (number_of_files > INT_MAX / (int)sizeof(struct music_file))
{
    fprintf(stderr, "Too many MP3 files to process safely.\n");
    curl_global_cleanup();
    return 1;
}

struct music_file *files =
    calloc((size_t)number_of_files, sizeof *files);

if (files == NULL) {
    perror("calloc");
    curl_global_cleanup();
    return 1;
}

int file_count = collect_music_files(
    folder_path,
    files,
    number_of_files
);

if (file_count <= 0)
{
    printf("No usable MP3 files found.\n");
    free(files);
    curl_global_cleanup();
    return 0;
}

normalize_build_a_problem_bonus_tracks(files, file_count);

/*
 * Sort everything before assigning album numbers.
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

        char preview_artist_path[PATH_MAX];
        char preview_album_path[PATH_MAX];
        char preview_cover_path[PATH_MAX];

        if (find_existing_directory_ci(
                folder_path,
                files[preview_start].artist,
                preview_artist_path,
                sizeof(preview_artist_path)) &&
            find_existing_directory_ci(
                preview_artist_path,
                files[preview_start].album,
                preview_album_path,
                sizeof(preview_album_path)) &&
            find_album_folder_jpg(
                preview_album_path,
                preview_cover_path,
                sizeof(preview_cover_path)))
        {
            printf(
                "  %s - %s: local artwork found: %s\n",
                files[preview_start].artist,
                files[preview_start].album,
                preview_cover_path
            );
        }
        else
        {
            printf(
                "  %s - %s: no album-local folder.jpg; "
                "MusicBrainz / Cover Art Archive fallback\n",
                files[preview_start].artist,
                files[preview_start].album
            );
        }

        preview_start = preview_end;
    }

    if (dry_run)
    {
        printf("\nPlanned file destinations (no files will be changed):\n");

        int dry_start = 0;
        int dry_album_number = 1;
        while (dry_start < file_count)
        {
            int dry_end = dry_start + 1;
            while (dry_end < file_count &&
                   strcasecmp(files[dry_start].artist, files[dry_end].artist) == 0 &&
                   strcasecmp(files[dry_start].album, files[dry_end].album) == 0)
            {
                dry_end++;
            }

            printf("\n----------------------------------------\n");
            printf("Album %d: %s - %s\n",
                   dry_album_number,
                   files[dry_start].artist,
                   files[dry_start].album);
            preview_album_tracks(folder_path, &files[dry_start],
                                 dry_end - dry_start);

            dry_album_number++;
            dry_start = dry_end;
        }

        printf("\nDry run complete. No files were changed.\n");
        free(files);
        curl_global_cleanup();
        return 0;
    }

    /*
     * Organize albums.
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

    free(files);
curl_global_cleanup();
return 0;}
