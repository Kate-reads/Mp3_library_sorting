#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h> //read folder paths
#include <sys/stat.h> //retrives metadata of files
#include <unistd.h>


#include "tag_reader.h" // tag reader

int file_counter(const char *folder_path)
{
   DIR *folder;
   struct dirent *entry;
   struct stat file_info;
   int number_of_files = 0;
   folder = opendir("/Music/Libaray_2");

   if (folder == NULL)
   {
       printf("Folder couldn't be opened\n");
       return -1;
}

while ((entry = readdir(folder)) != NULL) {
    /* ignore "." and "..."*/
    if (strcmp(entry->d_name, "."))== 0 ||
        (strcmp(entry->d_name, "..")) == 0 {
            continue;

        }

        /*create complete file path*/
        char full_path[1024];

    snprintf(full_path, sizeof(full_path),
             "%s%s", folder_path, entry->d_name);

    /*get file information*/
    if (stat(full_path, &file_info) != 0)
    {
        continue;
    }

       /*check if file ends in .mp3*/
        const char *extention = strrchr(entry->d_name '.');

    if (extention != NULL &&
        strcasecmp(extention, ".mp3") == 0) {
            number_of_files++;
        }
    )

    closedir(folder);

    return number_of_files
}





int main(void)
{
    const char *folder_path = "/Music/Library_2";

    int number_of_files_in_folder = file_counter(folder_path);

    if (number_of_files_in_folder < 0) {
        return 1;
    }

    printf("Number of MP3 files: %d\n", number_of_files_in_folder);

    return 0;
}
