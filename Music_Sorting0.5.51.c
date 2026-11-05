
Music_Sorting0.2.50.cCode

I have your updated Music_Sorting0.2.50.c file. What would you like me to do with this version?

I can continue from the previous debugging work, focusing on:

    Fix artist-folder duplication — make aurora and AURORA use the same artist folder.

    Fix album-folder duplication — keep tracks 10 and 11 of A Different Kind of Human together.

    Audit MP3/ID3 handling — identify potential metadata-reading errors and corruption risks.

    Review artwork handling — check Discogs downloads and embedded album art for safety issues.

    Prepare a safe test run — compile the program, test it on sample files, and verify that the original music library remains untouched.

My recommendation: Start with a full review of version 0.2.50, paying particular attention to folder grouping and file-moving safety before running it on your actual music library.
review

Reviewed code issues and assessed parser limitations

Searched files and summarized key findings

Compiled C file and removed stray ls command
Code review: Music_Sorting0.2.50.c

My review focuses on your music organizer's reliability, especially the two problems you've been working on: duplicate artist/album folders and safely handling MP3 metadata and album artwork.

Overall assessment: Version 0.2.50 has several good safety measures, but I would not run it on your main music library yet. There are still important issues with existing folder names, ID3 tag compatibility, and how the program handles large or unusual libraries.
1. Priority findings

High — Existing artist/album folder casing

The code searches for existing directories case-insensitively, which is a good improvement. However, organize_album() later constructs album_path using the capitalization from the MP3 tags rather than the actual directory path found by move_file().

For example, if the existing folder is AURORA but the tag says aurora, tracks may move into the existing folder while the subsequent artwork step looks for a lowercase aurora path. Artwork embedding can then fail.

High — Existing duplicate folders are not merged

find_existing_directory_ci() returns the first matching directory it finds. It does not merge two artist folders that already exist, or reconcile duplicate album directories.

This means the new logic can help prevent additional duplicates, but it does not repair the existing aurora / AURORA folders automatically.

High — ID3 parsing supports only part of the format

read_id3v2_tags() accepts ID3v2.3 only. It does not correctly handle all common text encodings, and its frame parsing does not account for every tag-level feature.

This can cause valid metadata to be missed or misread, leading to files being skipped or grouped incorrectly. The artwork writer deliberately rejects some unsupported tag flags, which is a useful safety measure, but the reader needs similar care.

Medium-high — The 1,000-file limit can silently omit music

MAX_TRACKS is set to 1000. The collector stops when it reaches that limit, but the main program does not compare the collected count against the total number of MP3 files.

If your source folder contains more than 1,000 tracks, some files will not be processed, even though the initial file count may be higher.

Good — Artwork embedding uses a temporary replacement

add_album_art() writes a temporary MP3, checks write and flush operations, calls fsync(), and then uses rename() to replace the original.

This is substantially safer than editing the original file in place. The routine also checks for existing artwork and refuses several unsupported tag structures instead of guessing.
2. Compilation check

I also ran a compiler syntax check against the uploaded source.

The uploaded file fails compilation as written.

The first line contains:

ls#define _POSIX_C_SOURCE 200809L

The stray ls before #define is invalid C and causes cascading compiler errors.

The syntax check passes after correcting that line.

I tested that correction in a temporary copy, leaving your uploaded file unchanged. This confirms the source passes the compiler's syntax and warning checks in that environment; it does not prove runtime behavior or metadata correctness.
3. Folder grouping and track numbering

Your code already uses strcasecmp() to compare artist and album names, so simple differences such as aurora versus AURORA should be grouped together during sorting.

However, there are three remaining issues:

    Existing duplicates: the code doesn't consolidate folders that already exist.

    Path consistency: the path used for artwork processing can differ in capitalization from the directory actually selected.

    Metadata variations: the normalization helper trims leading/trailing whitespace and handles several invisible UTF-8 characters, but it does not normalize every Unicode equivalent or repeated internal whitespace.

For A Different Kind of Human, the grouping logic should group album names that compare equal under strcasecmp(). If tracks 10 and 11 still end up separated, I would investigate their actual Artist and Album tag values and the paths constructed during processing, rather than assuming capitalization alone is the cause.

One additional detail: the program renumbers tracks sequentially as moves succeed. It does not preserve the original track numbers in the output filenames, and a skipped move does not increment the new track number.
4. MP3/ID3 handling

Issue


Potential effect


Recommended fix

Only ID3v2.3 is accepted


Other valid tag versions may be missed


Add safe support for v2.4 and, if needed, v2.2

UTF-8 and UTF-16BE text aren't properly decoded


Artist and album names can be lost or corrupted


Implement encoding-aware text decoding

Unsynchronization and extended headers aren't handled by the reader


Frames may be misread


Detect unsupported features and reject them safely, or implement their parsing

ID3v1 uses legacy text encoding


Non-ASCII metadata may be misinterpreted


Handle legacy encoding explicitly where practical

The artwork writer has several sensible safeguards: it checks for JPEG signatures, validates frame boundaries, preserves audio bytes by copying them into a temporary file, and avoids replacing an MP3 if the temporary write fails.

There are still improvements worth making. In particular, the replacement process should preserve the original file's permissions, check every close/read/write operation, and ideally retain a backup or provide a recovery strategy. An atomic rename protects against partial replacement, but it is not itself a backup.
5. File-moving and artwork risks

These are the other issues I would address before using the program on your library.

A. No dry-run mode

The main function proceeds directly to moving tracks and potentially embedding artwork. There is no command-line option to preview planned changes without applying them.

Recommendation: Add a dry-run mode that reports source paths, destination paths, album groupings, and artwork actions without creating directories, moving files, downloading images, or modifying MP3s.

B. Directory creation failures aren't propagated

create_directory() prints an error if mkdir() fails but returns no status. The caller may continue as if the directory were created, leading to confusing later failures.

Recommendation: Return success/failure and stop processing that track when directory creation fails.

C. The Discogs search can select the wrong release

The code verifies that the returned title contains the album name, but it doesn't strongly verify the artist or exact release. A compilation, reissue, or similarly named album could supply the wrong cover.

Recommendation: Score results against both artist and album, consider the year when available, and report the selected match for review. Validate downloaded image content before accepting it.

D. Artwork fallback may not find the album directory

After moving tracks, the code builds album_path from the sanitized metadata names instead of reusing the actual directory path selected by move_file(). This is the casing mismatch described earlier, and it affects both artwork downloads and embedding.

Recommendation: Resolve the artist and album paths once, retain the actual paths, and use them throughout the album operation.
6. Recommended repair order
Repair checklist

0 of 7

Correct the stray `ls` on line 1

Make the uploaded source compile.

Add dry-run mode

Prevent accidental moves and artwork writes during testing.

Use one canonical album path

Fix the artist/album casing mismatch.

Add robust directory error handling

Stop safely when a directory cannot be created.

Improve ID3 parsing

Handle supported encodings and tag versions correctly.

Add duplicate-folder reconciliation

Safely plan how existing duplicate folders will be merged.

Test artwork and recovery behavior

Use copies of real MP3s and verify audio, tags, and artwork afterward.
7. How I would test it safely

    Create a separate test directory containing copies of several MP3s, including tracks 10 and 11 from A Different Kind of Human.

    Include variations such as aurora and AURORA, plus an album with leading/trailing whitespace or different capitalization in its tags.

    Test missing tags, unusual ID3 encodings, existing destination filenames, and unsupported artwork tags.

    Run the program against the test directory only, after changing MUSIC_FOLDER to that test directory.

    Compare file counts, folder paths, track order, audio playback, and artwork before and after.

Do not test against /home/kate/Music/ until the dry-run mode and directory handling have been verified.

My conclusion: Version 0.2.50 is a useful foundation, and its temporary-file artwork writer is a meaningful safety improvement. But the compile error must be corrected, the actual directory path must be reused consistently, and a dry-run mode should come before any further live-library testing.

The best next step is to produce a corrected 0.2.51 that fixes these issues while preserving your existing organization rules and avoids changing MP3 tags unless explicitly intended.
This might also interest you.
fix issues
Worked for 1m 3s

I created Music_Sorting0.2.51.c with the main fixes from the review and tested the revised program.

Download the corrected C source file
What I fixed

    Compilation: Removed the stray ls at the start of the source file.

    Artist and album grouping: Normalizes repeated whitespace and handles capitalization differences, so aurora and AURORA can be grouped together.

    Album directory paths: Uses the actual existing directory path rather than reconstructing it with potentially different capitalization. This also helps artwork operations find the correct folder.

    Directory creation: Checks whether directory creation succeeds and stops the affected move if it fails.

    ID3 reading: Added basic support for ID3v2.4 frame sizes and UTF-8 text, while rejecting unsupported tag or frame flags instead of attempting to parse them incorrectly.

    Large libraries: Allocates space based on the actual MP3 count rather than silently stopping at 1,000 files.

    Safer testing: Added a dry-run mode and a configurable source folder, so you can test without modifying your main music library.

Test results

Compilation passed

Compiled with -Wall -Wextra -Wpedantic and no warnings or errors.

Dry-run test passed

Confirmed the program left the test MP3 files unchanged.

Grouping test passed

Two test tracks tagged with aurora and AURORA, and slightly different album whitespace, ended up in the same A Different Kind of Human album folder.
How to test it on your computer

Compile it first:

gcc -std=c11 -Wall -Wextra -Wpedantic \
    Music_Sorting0.2.51.c -o music_sorter \
    $(pkg-config --cflags --libs libcurl json-c)

Then run a dry run against a test copy of your music folder:

MUSIC_FOLDER="$HOME/Music-Test" MUSIC_SORT_DRY_RUN=1 ./music_sorter

If the preview looks correct, run it against that test copy without the dry-run setting:

MUSIC_FOLDER="$HOME/Music-Test" ./music_sorter

Important limitations: This version does not automatically merge duplicate artist or album folders that already exist. Merge operations can cause filename conflicts, so those should be handled separately and safely. The ID3 changes also do not provide complete support for every possible tag encoding or format. Keep a backup before testing actual file moves or artwork embedding.
Music_Sorting0.2.51.cCode
This might also interest you.


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

