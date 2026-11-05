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
 * Discogs requests should identify the application.
 *
 * Change this to your own application name/version if desired.
 */
#define DISCOGS_USER_AGENT "MusicSorter/0.2.52"

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


static const char *find_case_insensitive(
    const char *haystack,
    const char *needle)
{
    if (haystack == NULL || needle == NULL)
        return NULL;

    if (*needle == '\0')
        return haystack;

    for (const char *p = haystack; *p != '\0'; p++)
    {
        const char *h = p;
        const char *n = needle;

        while (*h != '\0' && *n != '\0' &&
               tolower((unsigned char)*h) ==
               tolower((unsigned char)*n))
        {
            h++;
            n++;
        }

        if (*n == '\0')
            return p;
    }

    return NULL;
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
                files[count].year,
                sizeof(files[count].year),
                genre,
                sizeof(genre),
                &track,
                &disc
            );

        /*
         * If ID3v2 exists but Artist/Album are missing,
         * try ID3v1 as a fallback.
         */
        if (!found ||
            files[count].artist[0] == '\0' ||
            files[count].album[0] == '\0')
        {
            char v1_artist[MAX_TEXT] = "";
            char v1_album[MAX_TEXT] = "";
            char v1_title[MAX_TEXT] = "";
            char v1_year[MAX_TEXT] = "";
            char v1_comment[64] = "";
            char v1_genre[64] = "";

            if (read_mp3_tags(
                    full_path,
                    v1_artist,
                    sizeof(v1_artist),
                    v1_album,
                    sizeof(v1_album),
                    v1_title,
                    sizeof(v1_title),
                    v1_year,
                    sizeof(v1_year),
                    v1_comment,
                    sizeof(v1_comment),
                    v1_genre,
                    sizeof(v1_genre)))
            {
                if (files[count].artist[0] == '\0')
                    snprintf(
                        files[count].artist,
                        sizeof(files[count].artist),
                        "%s",
                        v1_artist
                    );

                if (files[count].album[0] == '\0')
                    snprintf(
                        files[count].album,
                        sizeof(files[count].album),
                        "%s",
                        v1_album
                    );

                if (files[count].title[0] == '\0')
                    snprintf(
                        files[count].title,
                        sizeof(files[count].title),
                        "%s",
                        v1_title
                    );

                if (files[count].year[0] == '\0')
                    snprintf(
                        files[count].year,
                        sizeof(files[count].year),
                        "%s",
                        v1_year
                    );
            }
        }

        files[count].track_number = track;
        files[count].disc_number = disc;

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
   FIND LOCAL NUMBERED ARTWORK
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
    const char *filename,
    const char *token)
{
    FILE *file;

    file = fopen(filename, "wb");

    if (file == NULL)
    {
        perror("artwork output");
        return 0;
    }

    CURL *curl = curl_easy_init();

    if (curl == NULL)
    {
        fclose(file);
        return 0;
    }

    struct curl_slist *headers = NULL;

    char user_agent[256];

    snprintf(
        user_agent,
        sizeof(user_agent),
        "User-Agent: %s",
        DISCOGS_USER_AGENT
    );

    headers =
        curl_slist_append(
            headers,
            user_agent
        );

    if (token != NULL &&
        token[0] != '\0')
    {
        char authorization[512];

        snprintf(
            authorization,
            sizeof(authorization),
            "Authorization: Discogs token=%s",
            token
        );

        headers =
            curl_slist_append(
                headers,
                authorization
            );
    }

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        url
    );

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPHEADER,
        headers
    );

    curl_easy_setopt(
        curl,
        CURLOPT_FOLLOWLOCATION,
        1L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        file
    );

    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        30L
    );

    CURLcode result =
        curl_easy_perform(curl);

    long response_code = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &response_code
    );

    curl_slist_free_all(headers);

    curl_easy_cleanup(curl);

    fclose(file);

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

    if (response_code < 200 ||
        response_code >= 300)
    {
        fprintf(
            stderr,
            "Artwork download HTTP status: %ld\n",
            response_code
        );

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
   DISCOGS SEARCH
   ========================================================= */

static int find_discogs_artwork(
    const char *artist,
    const char *album,
    const char *year,
    const char *destination,
    size_t destination_size)
{
    (void)destination_size;

    const char *token =
        getenv("DISCOGS_TOKEN");

    if (token == NULL ||
        token[0] == '\0')
    {
        fprintf(
            stderr,
            "DISCOGS_TOKEN is not set.\n"
        );

        return 0;
    }

    char encoded_artist[1024];
    char encoded_album[1024];
    char encoded_year[128];

    if (!url_encode(
            artist,
            encoded_artist,
            sizeof(encoded_artist)))
        return 0;

    if (!url_encode(
            album,
            encoded_album,
            sizeof(encoded_album)))
        return 0;

    if (year != NULL &&
        year[0] != '\0')
    {
        if (!url_encode(
                year,
                encoded_year,
                sizeof(encoded_year)))
            return 0;
    }
    else
    {
        encoded_year[0] = '\0';
    }

    char url[PATH_MAX * 2];

    if (encoded_year[0] != '\0')
    {
        snprintf(
            url,
            sizeof(url),
            "https://api.discogs.com/database/search"
            "?artist=%s"
            "&release_title=%s"
            "&year=%s"
            "&type=release"
            "&per_page=10",
            encoded_artist,
            encoded_album,
            encoded_year
        );
    }
    else
    {
        snprintf(
            url,
            sizeof(url),
            "https://api.discogs.com/database/search"
            "?artist=%s"
            "&release_title=%s"
            "&type=release"
            "&per_page=10",
            encoded_artist,
            encoded_album
        );
    }

    printf(
        "Searching Discogs:\n"
        "  Artist: %s\n"
        "  Album:  %s\n"
        "  Year:   %s\n",
        artist,
        album,
        year != NULL && year[0] != '\0'
            ? year
            : "unknown"
    );

    CURL *curl = curl_easy_init();

    if (curl == NULL)
        return 0;

    struct memory_buffer response;

    response.data = NULL;
    response.size = 0;

    struct curl_slist *headers = NULL;

    char user_agent[256];

    snprintf(
        user_agent,
        sizeof(user_agent),
        "User-Agent: %s",
        DISCOGS_USER_AGENT
    );

    headers =
        curl_slist_append(
            headers,
            user_agent
        );

    char authorization[512];

    snprintf(
        authorization,
        sizeof(authorization),
        "Authorization: Discogs token=%s",
        token
    );

    headers =
        curl_slist_append(
            headers,
            authorization
        );

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        url
    );

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPHEADER,
        headers
    );

    curl_easy_setopt(
        curl,
        CURLOPT_FOLLOWLOCATION,
        1L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        curl_write_memory
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response
    );

    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        30L
    );

    CURLcode result =
        curl_easy_perform(curl);

    long response_code = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &response_code
    );

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK)
    {
        fprintf(
            stderr,
            "Discogs request failed: %s\n",
            curl_easy_strerror(result)
        );

        free(response.data);

        return 0;
    }

    if (response_code != 200)
    {
        fprintf(
            stderr,
            "Discogs HTTP status: %ld\n",
            response_code
        );

        free(response.data);

        return 0;
    }

    if (response.data == NULL)
        return 0;

    struct json_tokener *tokener =
        json_tokener_new();

    if (tokener == NULL)
    {
        free(response.data);
        return 0;
    }

    struct json_object *root =
        json_tokener_parse_ex(
            tokener,
            (const char *)response.data,
            (int)response.size
        );

    enum json_tokener_error json_error =
        json_tokener_get_error(tokener);

    json_tokener_free(tokener);

    free(response.data);

    if (json_error != json_tokener_success ||
        root == NULL)
    {
        fprintf(
            stderr,
            "Could not parse Discogs JSON.\n"
        );

        if (root != NULL)
            json_object_put(root);

        return 0;
    }

    struct json_object *results = NULL;

    if (!json_object_object_get_ex(
            root,
            "results",
            &results))
    {
        json_object_put(root);
        return 0;
    }

    size_t result_count =
        json_object_array_length(results);

    for (size_t i = 0; i < result_count; i++)
    {
        struct json_object *item =
            json_object_array_get_idx(
                results,
                i
            );

        if (item == NULL)
            continue;

        struct json_object *title_obj = NULL;
        struct json_object *year_obj = NULL;
        struct json_object *id_obj = NULL;
        struct json_object *uri_obj = NULL;
        struct json_object *cover_obj = NULL;

        const char *result_title = "";
        const char *result_year = "";
        const char *cover_url = "";

        if (json_object_object_get_ex(
                item,
                "title",
                &title_obj))
        {
            result_title =
                json_object_get_string(title_obj);
        }

        if (json_object_object_get_ex(
                item,
                "year",
                &year_obj))
        {
            result_year =
                json_object_get_string(year_obj);
        }

        if (json_object_object_get_ex(
                item,
                "id",
                &id_obj))
        {
            /* Used below. */
        }

        if (json_object_object_get_ex(
                item,
                "uri",
                &uri_obj))
        {
            /* Used below. */
        }

        /*
         * Discogs search results normally provide a
         * "cover_image" URL. We only accept results
         * that actually have one.
         */
        if (!json_object_object_get_ex(
                item,
                "cover_image",
                &cover_obj))
        {
            continue;
        }

        cover_url =
            json_object_get_string(cover_obj);

        if (cover_url == NULL ||
            cover_url[0] == '\0')
            continue;

        /*
         * The search title generally includes
         * "Artist - Album".
         *
         * Verify the album name appears in the
         * returned title before accepting it.
         */
        if (find_case_insensitive(
                result_title,
                album) == NULL)
        {
            continue;
        }

        if (json_object_object_get_ex(
                item,
                "id",
                &id_obj))
        {
            printf(
                "Discogs match:\n"
                "  %s\n"
                "  Year: %s\n"
                "  Release ID: %s\n",
                result_title,
                result_year,
                json_object_get_string(id_obj)
            );
        }
        else
        {
            printf(
                "Discogs match:\n"
                "  %s\n"
                "  Year: %s\n",
                result_title,
                result_year
            );
        }

        if (json_object_object_get_ex(
                item,
                "uri",
                &uri_obj))
        {
            printf(
                "  Release: %s\n",
                json_object_get_string(uri_obj)
            );
        }

        /*
         * Save the image temporarily first.
         */

        if (!download_file(
                cover_url,
                destination,
                token))
        {
            continue;
        }

        printf(
            "Discogs artwork downloaded:\n"
            "  %s\n",
            destination
        );

        printf(
            "Data provided by Discogs.\n"
        );

        json_object_put(root);

        /*
         * Small delay between API operations.
         */
        sleep(1);

        return 1;
    }

    json_object_put(root);

    printf(
        "No suitable Discogs artwork found.\n"
    );

    sleep(1);

    return 0;
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

    if (memcmp(header, "ID3", 3) != 0 || header[3] != 3)
    {
        fprintf(
            stderr,
            "Skipping artwork for %s: requires an ID3v2.3 tag.\n",
            mp3_filename
        );
        goto cleanup;
    }

    /*
     * Refuse tag variants that this small writer cannot safely preserve.
     * In particular, unsynchronisation and the v2.3 extended header need
     * special handling; silently rewriting them would be riskier than
     * leaving the original MP3 untouched.
     */
    if ((header[5] & 0xC0) != 0)
    {
        fprintf(
            stderr,
            "Skipping artwork for %s: unsupported ID3v2.3 flags (0x%02X).\n",
            mp3_filename,
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

        unsigned int frame_size =
            ((unsigned int)old_tag[position + 4] << 24) |
            ((unsigned int)old_tag[position + 5] << 16) |
            ((unsigned int)old_tag[position + 6] << 8) |
            (unsigned int)old_tag[position + 7];

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
            "Artwork already embedded; leaving existing APIC unchanged: %s\n",
            mp3_filename
        );
        success = 1;
        goto cleanup;
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
        apic_size > SIZE_MAX - 10 ||
        old_tag_size > SIZE_MAX - 10 - apic_size)
        goto cleanup;

    size_t apic_total = 10 + apic_size;
    new_tag_size = old_tag_size + apic_total;

    if (new_tag_size > 0x0FFFFFFFU)
        goto cleanup;

    new_tag = malloc(new_tag_size);
    if (new_tag == NULL)
        goto cleanup;

    memcpy(new_tag, old_tag, insert_position);

    position = insert_position;
    memcpy(&new_tag[position], "APIC", 4);
    new_tag[position + 4] = (unsigned char)((apic_size >> 24) & 0xFF);
    new_tag[position + 5] = (unsigned char)((apic_size >> 16) & 0xFF);
    new_tag[position + 6] = (unsigned char)((apic_size >> 8) & 0xFF);
    new_tag[position + 7] = (unsigned char)(apic_size & 0xFF);
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
        old_tag_size - insert_position
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

    if (success)
    {
        printf(
            "Artwork embedded safely: %s\n",
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
   MOVE ARTWORK
   ========================================================= */

static int move_album_art(
    const char *source_path,
    const char *album_path)
{
    char destination[PATH_MAX];

    int length =
        snprintf(
            destination,
            sizeof(destination),
            "%s/folder.jpg",
            album_path
        );

    if (length < 0 ||
        (size_t)length >= sizeof(destination))
        return 0;

    if (access(destination, F_OK) == 0)
    {
        fprintf(
            stderr,
            "Artwork already exists, not overwriting:\n"
            "  %s\n",
            destination
        );

        return 0;
    }

    if (rename(
            source_path,
            destination) != 0)
    {
        perror("rename artwork");
        return 0;
    }

    printf(
        "Artwork moved:\n"
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

    qsort(
        files,
        count,
        sizeof(struct music_file),
        compare_music_files
    );

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
     * ARTWORK
     *
     * Local folderN.jpg has priority.
     *
     * If absent, query Discogs.
     * ======================================================
     */

    char cover_path[PATH_MAX];
    int have_cover = 0;
    int local_cover = 0;

    cover_path[0] = '\0';

    if (find_numbered_folder_jpg(
            base_path,
            album_number,
            cover_path,
            sizeof(cover_path)))
    {
        have_cover = 1;
        local_cover = 1;

        printf(
            "Artwork: local\n"
            "  %s\n",
            cover_path
        );
    }
    else
    {
        printf(
            "Artwork: local artwork not found.\n"
        );
    }


    /*
     * ======================================================
     * MOVE TRACKS
     * ======================================================
     */

    int fallback_track_number = 1;

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

        /*
         * Preserve the original ID3 track number in the filename.
         * If the ID3 track number is missing, use a leading number
         * from the original filename (for example, "03 - Title.mp3").
         * Only use a fallback sequence when neither is available.
         */
        int filename_track_number = files[i].track_number;

        if (filename_track_number <= 0)
        {
            const char *p = files[i].filename;

            while (*p != '\0' && isspace((unsigned char)*p))
                p++;

            if (isdigit((unsigned char)*p))
                filename_track_number = atoi(p);
        }

        if (filename_track_number <= 0)
        {
            filename_track_number = fallback_track_number++;
        }

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
                "Disc %d Track %d -> Filename Track %d\n",
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
     * ======================================================
     * DISCogs fallback
     *
     * Do this AFTER moving tracks, but BEFORE moving
     * local folderN.jpg.
     * ======================================================
     */

    if (!have_cover)
    {
        char downloaded_cover[PATH_MAX];

        length =
            snprintf(
                downloaded_cover,
                sizeof(downloaded_cover),
                "%s/.discogs-cover.jpg",
                album_path
            );

        if (length >= 0 &&
            (size_t)length < sizeof(downloaded_cover))
        {
            if (find_discogs_artwork(
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
                    "Artwork source: Discogs\n"
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
         * Move folderN.jpg → folder.jpg
         * ==================================================
         */

        if (local_cover)
        {
            move_album_art(
                cover_path,
                album_path
            );
        }
        else
        {
            /*
             * Discogs image was already downloaded directly
             * as .discogs-cover.jpg.
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
                            "Discogs artwork saved:\n"
                            "  %s\n",
                            final_cover
                        );
                    }
                    else
                    {
                        perror(
                            "rename Discogs artwork"
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

    const char *discogs_token =
        getenv("DISCOGS_TOKEN");

    if (discogs_token != NULL &&
        discogs_token[0] != '\0')
    {
        printf(
            "Discogs artwork fallback: ENABLED\n"
        );
    }
    else
    {
        printf(
            "Discogs artwork fallback: DISABLED\n"
            "Set DISCOGS_TOKEN to enable it.\n"
        );
    }

    printf(
        "Discogs API attribution: "
        "This application uses Discogs' API but is not "
        "affiliated with, sponsored or endorsed by Discogs.\n"
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

    if (dry_run)
    {
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
