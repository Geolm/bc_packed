#include "test_helpers.h"
#include <dirent.h>
#include <stdio.h>
#include <time.h>

//-----------------------------------------------------------------------------------------------------------------------------
FILE* open_hourly_stats_file(const char *folder_path)
{
    if (!folder_path) return NULL;

    time_t now = time(NULL);
    if (now == (time_t)(-1)) return NULL;

    struct tm *t = localtime(&now);
    if (t == NULL) return NULL;

    // 1. Format the filename
    char filename[64];
    if (strftime(filename, sizeof(filename), "stats_%Y-%m-%d_%H.txt", t) == 0) return NULL;

    // 2. Build the full path
    char full_path[1024];
    int len = snprintf(full_path, sizeof(full_path), "%s/%s", folder_path, filename);
    
    if (len < 0 || (size_t)len >= sizeof(full_path)) return NULL;

    // 3. Open the file in append mode
    return fopen(full_path, "a");
}

//-----------------------------------------------------------------------------------------------------------------------------
TEST test_all_images_in_folder(const char *dir_path) 
{
    DIR *dir = opendir(dir_path);
    if (!dir) 
    {
        fprintf(stderr, "Unable to open directory %s\n", dir_path);
        FAIL();
    }

    FILE* stats = open_hourly_stats_file("./logs");
    if (!stats)
    {
        fprintf(stderr, "Unable to create stats file\n");
        FAIL();
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) 
    {
        // Skip current (".") and parent ("..") directory pointers
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) 
        {
            continue;
        }

        // Construct full file path
        char file_path[1024];
        snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, entry->d_name);

        // Attempt to open the file
        FILE *file = fopen(file_path, "rb");
        if (!file)
            continue;
        

        fprintf(stats, "open file %s", file_path);

        (void)roundtrip;

        // Process file contents here...

        fclose(file);
    }

    closedir(dir);

    fclose(stats);

    PASS();
}


SUITE(suite_image)
{
    RUN_TEST1(test_all_images_in_folder, "./images/");
}


