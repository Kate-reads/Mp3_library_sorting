#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h> //read folder paths
#include <sys/stat.h> //retrives metadata of files
#include <unistd.h>

int file_counter(const char *folder_path)
{
   DIR *folder;
   struct dirent *entry;
   int number_of_files = 0;
   folder = opendir(/home/kate/Music/Libaray_2);

   if (folder = NULL)
   {
       printf("Folder couldn't be opened\n")
       return -1;
}

while ((entry = readir(folder)) != NULL) {
    /* ignore "." and "..."*/
    if (strcmp->d_name, ".") == 0
        (strcmp->d_name, "..") == 0 {
            continue;

        }

        /*check if file ends in .mp3*/
        const char *extention = strrchar(entry->d_name ".");

    if (extention =! NULL &&
        strcasecmp(extention, ".mp3") == 0) {
            number_of_files++
        }
    )

    closedir(folder);

    return number_of_files
}





int main(void)
{
    const char *folder_path = ".";
    int number_of_files_in_folder = file_counter(file_path);

    if (number_of_files < 0) {
        return 1;
    }

    printf("Number of MP3 files: %d\n", number_of_files_in_folder);

    return 0;
}
