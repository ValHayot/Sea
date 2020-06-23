#include "config.h"
#include "logger.h"
#include "sea.h"
#include "passthrough.h"
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <cstdlib>

int sea_internal;

std::vector<char *> sea_files;

/**
 * Getter for the sea_internal global variable
 *
 * @return the value of sea_internal
 */
int get_internal() {
    return sea_internal;
}

/**
 * Setter for the sea_internal global variable. Always sets it to 1.
 *
 * @return the value of sea_internal
 */
int set_internal() {
    sea_internal = 1;
    return sea_internal;
}


/**
 * Unsets the sea_internal global variable (i.e. sets it to 0).
 *
 * @return the value of sea_internal
 */
int unset_internal() {
    sea_internal = 0;
    return sea_internal;
}

/**
 * Checks if path exists by performing an __xstat
 *
 * @param path the path to verify
 * @return 1 if path exists or 0 if it does not
 */
int sea_checkpath(const char* path) {
    struct stat buf;

    errno = 0;
    //printf("checkpath %s\n", path);
    set_internal();
    int ret = 0;
    if((ret = __xstat(_STAT_VER_LINUX, path, &buf)) != 0) {
        //printf("errno %d\n", errno);
        return 0;
    }
    unset_internal();
    return 1;
}

/**
 *
 * Overloaded function of sea_getpath. Returns the true path of the file if located within the mountpoint.
 * Can also return the "masked" (path relative to mountpoint).
 *
 * @param oldpath the original function path
 * @param passpath the true path of the file (if located in a sea mountpoint)
 * @param masked_path whether to populate passpath with the real source mount location or with the "masked" mountpoint path
 * @return whether oldpath was located in a sea mountpoint or not.
 *
 */
int sea_getpath(const char* oldpath, char passpath[PATH_MAX], int masked_path) {

    return sea_getpath(oldpath, passpath, masked_path, -1);
}

/**
 *
 * "Main" overloaded function that calls passthrough_getpath to get real path.
 * Returns the true path of the file if located within the mountpoint.
 * Can also return the "masked" (path relative to mountpoint).
 *
 * @param oldpath the original function path
 * @param passpath the true path of the file (if located in a sea mountpoint)
 * @param masked_path whether to populate passpath with the real source mount location or with the "masked" mountpoint path
 * @param sea_lvl specifies the index of the source mount to use. If -1, will go through all possible source_mounts to look for existing path
 * @return whether oldpath was located in a sea mountpoint or not.
 *
 */
int sea_getpath(const char* oldpath, char passpath[PATH_MAX], int masked_path, int sea_lvl) {
    struct config sea_config = get_sea_config();

    int match = 0;
    char tmp_passpath[PATH_MAX] = { '\0' };

    if (sea_lvl != -1) {
        match = pass_getpath(oldpath, passpath, masked_path, sea_lvl);
        //printf("match %d %s\n", match, passpath);
    }
    else {
        for ( int i=0 ; i < sea_config.n_sources; ++i ) {
            match = pass_getpath(oldpath, passpath, masked_path, i);
            //printf("passpath %s\n", passpath);

            if ( masked_path == 0 && match == 1) {
                int exists = sea_checkpath(passpath);
                //printf("exists %d\n", exists);

                if (exists)
                    return match; 
                else if (tmp_passpath[0] == '\0') {
                    // if doesn't exist, create at top of hierarchy
                    strcpy(tmp_passpath, passpath);
                }
            }
            else if ( masked_path == 1 ) {
                return match;
            }
        }
    }

    if (match == 1 && tmp_passpath[0] != '\0')
        strcpy(passpath, tmp_passpath);
    
    if (match == 0) {
        strcpy(passpath, oldpath);
    }

    //printf("returned passpath %s\n", passpath);
    
    return match;

}

// obtained from : https://codeforwin.org/2018/03/c-program-to-list-all-files-in-a-directory-recursively.html
// modified to populate vector
/**
 * Populate a vector with all files and directories located within a given source mount.
 * Directories which do not exist in all source mounts are created in all the mounts.
 *
 * @param basePath the root directory to start adding paths from
 * @param sea_lvl the index of the basePath's parent source mount
 * @param sea_config the sea configuration struct
 * @param path_vec the reference to a vector where paths will be appended to
 *
 */
void populateFileVec(char *basePath, int sea_lvl, struct config sea_config, std::vector<char *> &path_vec)
{
    //printf("base %s \n", basePath);
    char path[PATH_MAX];
    struct dirent *dp;
    DIR *dir = ((funcptr_opendir)libc_opendir)(basePath);

    // Unable to open directory stream
    if (!dir)
        return;

    while ((dp = ((funcptr_readdir)libc_readdir)(dir)) != NULL)
    {

        // Construct new path from our base path
        strcpy(path, basePath);

        //if (path[strlen(path) - 1] != '/')
        strcat(path, "/");
        strcat(path, dp->d_name);
        //printf("adding file %s\n", path);
        char* fp = new char[PATH_MAX];
        memcpy(fp, path, PATH_MAX);

        path_vec.push_back(fp);

        if (strcmp(dp->d_name, ".") != 0 && strcmp(dp->d_name, "..") != 0)
        {
            
            if (dp->d_type == DT_DIR) {
                char dir_to_create[PATH_MAX];

                struct stat buf;
                if( ((funcptr___xstat)libc___xstat)(_STAT_VER_LINUX, fp, &buf) == 0) {
                    //printf("adding file %s\n", dp->d_name);

                    for (int i=0; i < sea_config.n_sources; ++i) {
                        if (i != sea_lvl) {
                            strcpy(dir_to_create, sea_config.source_mounts[i]);
                            //if (dir_to_create[strlen(dir_to_create) - 1] != '/')
                            strcat(dir_to_create, "/");
                            strcat(dir_to_create, dp->d_name);

                            //TODO: add error handling here
                            ((funcptr_mkdir)libc_mkdir)(dir_to_create, buf.st_mode);
                        }
                    } 
                }
            }
            populateFileVec(fp, sea_lvl, sea_config, path_vec);
        }
    }
    closedir(dir);
}

/**
 * Populate the sea_files vector with all the directories and paths located within the
 * source mounts
 *
 */
void initialize_sea() {
    sea_internal = 0;
    struct config sea_config = get_sea_config();
    char ** source_mounts = sea_config.source_mounts;

    for (int i=0; i < sea_config.n_sources; i++){
        populateFileVec(source_mounts[i], i, sea_config, sea_files);
    }
    //for (auto file : sea_files)
    //    printf("sea file %s\n", file);
}

static pthread_once_t sea_initialized = PTHREAD_ONCE_INIT;

/**
 * Create a pthread to intialize the sea_files vector. Doesn't really work, so also
 * check if there are no files in the sea_files vector and if so, call initialize_sea 
 *
 */
void initialize_sea_if_necessary() {
  pthread_once(&sea_initialized, initialize_sea);

  if (sea_files.size() == 0)
        initialize_sea();
}
