#define _XOPEN_SOURCE 500
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <ftw.h>

#define ERR(source) (perror(source),\
             fprintf(stderr,"%s:%d\n",__FILE__,__LINE__), \
             exit(EXIT_FAILURE))

#define MAXFD 20
#define FILENAME_LENGTH 100
#define PATH_LENGTH 300
#define MAX_INPUT_LENGTH 100

#define DATABASE_GROWTH_FACTOR 10
#define DATABASE_INITIAL_CAPACITY 100

void usage(char *name) {
    fprintf(stderr, "USAGE: %s -d [path] -f [index-path] -t [time-interval]\n", name);
    fprintf(stderr, "d - path do directory traversed, if not provided $MOLE_DIR is used\n");
    fprintf(stderr, "\tthis argument or env variable is required for program to start\n");
    fprintf(stderr, "m - path to storage of index file, if not present $MOLE_INDEX_PATH is used,\n");
    fprintf(stderr, "\tif $MOLE_INDEX_PATH not present file is stored in $HOME/.mole-index\n");
    fprintf(stderr, "t - value from range [30,7200], when provided enables rebuilding of index at given interval\n");
    exit(EXIT_FAILURE);
}

typedef struct indexedFile_s {
    char fileName[FILENAME_LENGTH + 1];
    char path[PATH_LENGTH + 1];
    off_t size;
    uid_t UID;
    char fileType[8];
} indexedFile;

typedef struct threadData_s {
    pthread_t threadID;
    time_t lastIndexingTime;
    time_t fileLastModificationTime;
    char *m;
    char *d;
    int t;
    int *indexingFlag;
    int *mFlag;
    pthread_mutex_t *databaseMutex;
    pthread_mutex_t *indexingFlagMutex;
    struct threadData_s *indexingThread;
} threadData;

typedef struct database_s {
    indexedFile *database;
    int databaseSize;
    int currentIdx;
    int fileDescriptor;
} database;

typedef struct globalVar_s {
    int reindexingFlag;
    database mainDatabase;
    database tempDatabase;
} globalStructure;

globalStructure global;

void readArguments(int argc, char **argv, char **d, char **m, int *t, int *mFlag) {
    if (argc > 7) usage(argv[0]);
    int c;

    while ((c = getopt(argc, argv, "d:m:t:")) != -1)
        switch (c) {
            case 'd':
                if (optarg[0] == '-') {
                    fprintf(stderr, "Option -%c requires an argument.\n", c);
                    usage(argv[0]);
                }
                *d = optarg;
                break;
            case 'm':
                if (optarg[0] == '-') {
                    fprintf(stderr, "Option -%c requires an argument.\n", c);
                    usage(argv[0]);
                }
                *m = optarg;
                break;
            case 't':
                *t = atoi(optarg);
                if (*t < 30 || *t > 7200) {
                    fprintf(stderr, "Incorrect value for -%c argument.\n", c);
                    usage(argv[0]);
                }
                break;
            case '?':
                if (optopt == 'd' || optopt == 'm' || optopt == 't') {
                    fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                } else if (isprint (optopt))
                    fprintf(stderr, "Unknown option `-%c'.\n", optopt);
                else
                    fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                usage(argv[0]);
                break;
            default:
                usage(argv[0]);
        }

    if (*d == NULL) {
        *d = getenv("MOLE_DIR");
        if (d == NULL) {
            fprintf(stderr, "$MOLE_DIR not present, please provide path as argument of env variable\n");
            usage(argv[0]);
        }
    }

    if (*m == NULL) {
        *m = getenv("MOLE_INDEX_PATH");
        if (*m == NULL) {
            *mFlag = 1;
            char *path = malloc(strlen(getenv("HOME")) + strlen("/.mole-index") + 1);
            if(path == NULL) ERR("malloc");
            strcpy(path, getenv("HOME"));
            *m = strcat(path, "/.mole-index");
        }
    }
}

void freeResources(threadData *data) {
    pthread_mutex_destroy(data->databaseMutex);
    pthread_mutex_destroy(data->indexingFlagMutex);
    if (*data->mFlag) {
        free(data->m);
    }
}

// helper method for handling index file
void resizeFile(database *db) {
    int result = ftruncate(db->fileDescriptor, db->databaseSize * sizeof(indexedFile));
    if (result == -1) {
        close(db->fileDescriptor);
        perror("Error calling lseek() to 'stretch' the file");
        exit(EXIT_FAILURE);
    }
}

// helper method to map file
void mapFile(database *db) {
    db->database = (indexedFile *) mmap(NULL, sizeof(indexedFile) * db->databaseSize, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, db->fileDescriptor, 0);
    if (db->database == MAP_FAILED) {
        close(db->fileDescriptor);
        perror("Error mmapping the file!\n");
        exit(EXIT_FAILURE);
    }
}

void databaseResize(database *db) {
    // unmapping file
    if (munmap(db->database, db->databaseSize * sizeof(indexedFile)) == -1)
        perror("Error unmapping");

    // resizing database and file
    db->databaseSize *= DATABASE_GROWTH_FACTOR;
    resizeFile(db);

    // mapping file of extended size
    mapFile(db);
}

// reading magic number
char *magicNumber(const char *name) {
    int temp = open(name, O_RDONLY, 0);
    if (temp < 0) {
        perror("error while indexing!");
    }
    unsigned char bytes[3];
    read(temp, bytes, 3);
    close(temp);

    if (bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) return "jpeg";
    if (bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4e) return "png";
    if (bytes[0] == 0x50 && bytes[1] == 0x4b) return "zip";
    if (bytes[0] == 0x1f && bytes[1] == 0x8b) return "gzip";

    return NULL;
}

// method for nftw function - this is where indexing happens
int directoryWalk(const char *name, const struct stat *s, int type, struct FTW *Sf) {
    database *db;
    // check if indexing or reindexing and populate correct database
    if (global.reindexingFlag) {
        db = &global.tempDatabase;
    } else {
        db = &global.mainDatabase;
    }

    indexedFile *database = db->database;
    int *workingIdx = &db->currentIdx;

    // if directory omit magic number check
    if (type == FTW_D) {
        strcpy(database[*workingIdx].fileType, "0");
    }

    // check magic number of file
    if (type == FTW_F) {
        char *fileType = magicNumber(name);
        if (fileType != NULL) {
            strcpy(database[*workingIdx].fileType, fileType);
        } else {
            return 0;
        }
    }

    // copy filename out of full path
    char *currentName = malloc(strlen(name) + 1);
    if(currentName == NULL) ERR("malloc");
    memcpy(currentName, name, strlen(name) + 1);

    char *p = strrchr(name, '/');
    char *filename = p + 1;
    if (strlen(filename) > FILENAME_LENGTH) {
        fprintf(stdout, "filename length over limit! cropping to %d characters", FILENAME_LENGTH);
        strncpy(database[*workingIdx].fileName, filename, FILENAME_LENGTH);
        database[*workingIdx].fileName[FILENAME_LENGTH] = '\0';
    }
    strcpy(database[*workingIdx].fileName, filename);

    // copy path
    if (strlen(currentName) > PATH_LENGTH) {
        fprintf(stdout, "path length over limit! cropping to %d characters", PATH_LENGTH);
        strncpy(database[*workingIdx].path, currentName, PATH_LENGTH);
        database[*workingIdx].path[PATH_LENGTH] = '\0';
    }
    strcpy(database[*workingIdx].path, currentName);
    free(currentName);

    // uid and file size
    database[*workingIdx].UID = s->st_uid;
    database[*workingIdx].size = s->st_size;

    // moving to the next entry & resize file if needed
    (*workingIdx)++;
    if (*workingIdx == db->databaseSize) {
        databaseResize(db);
    }

    return 0;
}

char *getTempFilePath(threadData *data) {
    char *tempFilePath = malloc(strlen(data->m) + 6);
    if(tempFilePath == NULL) ERR("malloc");
    memcpy(tempFilePath, data->m, strlen(data->m) + 1);
    strcat(tempFilePath, "-temp");
    return tempFilePath;
}

void swapFiles(threadData *data, char *tempfile) {
    char *tempFilePath = tempfile;

    // remove old file
    int del = remove(data->m);
    if (del) {
        perror("Error deleting old database. Aborting!\n");
        free(tempFilePath);
        pthread_mutex_unlock(data->databaseMutex);
        pthread_mutex_lock(data->indexingFlagMutex);
        *data->indexingFlag = 0;
        global.reindexingFlag = 0;
        pthread_mutex_unlock(data->indexingFlagMutex);
        return;
    }

    // rename new file
    int ren = rename(tempFilePath, data->m);
    if (ren) {
        perror("Error renaming new database. Exiting!\n");
        free(tempFilePath);
        pthread_mutex_unlock(data->databaseMutex);
        freeResources(data);
        exit(EXIT_FAILURE);
    }
}

void shutdownProcedure(void *voidPtr) {
    threadData *data = voidPtr;

    // unmap database
    if (global.mainDatabase.database != NULL) {
        if (munmap(global.mainDatabase.database, global.mainDatabase.databaseSize * sizeof(indexedFile)) == -1)
            perror("error unmapping");
        close(global.mainDatabase.fileDescriptor);
    }

    // if there was reindexing in process
    if (global.reindexingFlag) {
        // unmap temp database
        if (munmap(global.tempDatabase.database, global.tempDatabase.databaseSize * sizeof(indexedFile)) == -1)
            perror("error unmapping");

        // close temp file
        close(global.tempDatabase.fileDescriptor);
        char *tempFilePath = getTempFilePath(data);
        swapFiles(data, tempFilePath);
        free(tempFilePath);
    }

    freeResources(data);
}

// worker method for thread responsible for indexing files
void *indexFiles(void *voidPtr) {
    threadData *data = (threadData *) voidPtr;
    pthread_cleanup_push(shutdownProcedure, voidPtr) ;
    time(&data->lastIndexingTime);

    pthread_mutex_lock(data->databaseMutex);
    global.mainDatabase.currentIdx = 0;

    nftw(data->d, directoryWalk, MAXFD, FTW_PHYS);

    pthread_mutex_unlock(data->databaseMutex);
    fprintf(stdout, "Indexing finished!\n");

    pthread_mutex_lock(data->indexingFlagMutex);
    *data->indexingFlag = 0;
    pthread_mutex_unlock(data->indexingFlagMutex);
    pthread_cleanup_pop(0);
    return NULL;
}

// method to find how many entries there are in index file after loading it for the first time
void findLastIndex() {
    int i = 0;
    while (global.mainDatabase.database[i].size != 0) {
        i++;
    }
    global.mainDatabase.currentIdx = i;
}

void openFile(char *m, threadData *indexingThread) {
    global.mainDatabase.fileDescriptor = open(m, O_RDWR, (mode_t) 0660);
    if (global.mainDatabase.fileDescriptor < 0) {
        return;
    }

    // loading file stats to determine size of database and save mod time
    struct stat fileStats;
    fstat(global.mainDatabase.fileDescriptor, &fileStats);
    global.mainDatabase.databaseSize = fileStats.st_size / sizeof(indexedFile);
    indexingThread->fileLastModificationTime = fileStats.st_mtim.tv_sec;

    mapFile(&global.mainDatabase);
    findLastIndex();
}

void createFile(char *m, threadData *indexingThread) {
    global.mainDatabase.fileDescriptor = open(m, O_RDWR | O_CREAT, (mode_t) 0660);
    if (global.mainDatabase.fileDescriptor < 0) {
        perror("Error creating file. Exiting!\n");
        exit(EXIT_FAILURE);
    }

    // set size, resize file and map it
    global.mainDatabase.databaseSize = DATABASE_INITIAL_CAPACITY;
    resizeFile(&global.mainDatabase);
    mapFile(&global.mainDatabase);

    // start indexing
    pthread_mutex_lock(indexingThread->indexingFlagMutex);
    *indexingThread->indexingFlag = 1;
    pthread_mutex_unlock(indexingThread->indexingFlagMutex);
    int err = pthread_create(&indexingThread->threadID, NULL, indexFiles, indexingThread);
    if (err != 0) ERR("pthread_create");
}

void finishReindexing(threadData *data, char *tempFilePath) {
    free(tempFilePath);
    pthread_mutex_unlock(data->databaseMutex);
    pthread_mutex_lock(data->indexingFlagMutex);
    *data->indexingFlag = 0;
    global.reindexingFlag = 0;
    pthread_mutex_unlock(data->indexingFlagMutex);
}

void *reindexFiles(void *voidPtr) {
    printf("Starting reindexing!\n");
    threadData *data = voidPtr;
    pthread_mutex_lock(data->indexingFlagMutex);
    global.reindexingFlag = 1;
    *data->indexingFlag = 1;
    pthread_mutex_unlock(data->indexingFlagMutex);
    pthread_cleanup_push(shutdownProcedure, voidPtr);

    // init tempDatabase variables
    global.tempDatabase.databaseSize = global.mainDatabase.databaseSize;
    global.tempDatabase.currentIdx = 0;

    // creating temporary file
    char *tempFilePath = getTempFilePath(data);
    global.tempDatabase.fileDescriptor = open(tempFilePath, O_RDWR | O_CREAT, (mode_t) 0660);
    if (global.tempDatabase.fileDescriptor < 0) {
        perror("Error creating temporary file. Aborting!\n");
        free(tempFilePath);
        return NULL;
    }

    // resize, map & index temp db
    resizeFile(&global.tempDatabase);
    mapFile(&global.tempDatabase);
    nftw(data->d, directoryWalk, MAXFD, FTW_PHYS);

    // lock database
    pthread_mutex_lock(data->databaseMutex);

    // unmap and close old file
    if (munmap(global.mainDatabase.database, global.mainDatabase.databaseSize * sizeof(indexedFile)) == -1) {
        perror("Error unmapping old database. Aborting!\n");
        finishReindexing(data, tempFilePath);
        return NULL;
    }
    close(global.mainDatabase.fileDescriptor);

    // swap files and databases
    swapFiles(data, tempFilePath);
    global.mainDatabase = global.tempDatabase;

    // exit
    finishReindexing(data, tempFilePath);
    fprintf(stdout, "Reindexing finished!\n");
    pthread_cleanup_pop(0);
    return NULL;
}

void *periodicIndexing(void *voidPtr) {
    threadData *data = voidPtr;
    threadData *indexingThread = data->indexingThread;

    while (1) {
        time_t currentTime;
        time(&currentTime);

        // compare current time with last modification or lastindexing time
        if ((indexingThread->lastIndexingTime == 0 &&
             difftime(currentTime, indexingThread->fileLastModificationTime) > data->t)
            || (indexingThread->lastIndexingTime != 0 &&
                difftime(currentTime, indexingThread->lastIndexingTime) > data->t)) {
            pthread_mutex_lock(indexingThread->indexingFlagMutex);
            if (*data->indexingFlag == 0) {
                pthread_mutex_unlock(indexingThread->indexingFlagMutex);
                time(&data->indexingThread->lastIndexingTime);
                int err = pthread_create(&indexingThread->threadID, NULL, reindexFiles, indexingThread);
                if (err != 0) ERR("pthread_create");
            }
            pthread_mutex_unlock(indexingThread->indexingFlagMutex);
        }

        time_t time;
        if (indexingThread->lastIndexingTime == 0) {
            time = indexingThread->fileLastModificationTime;
        } else {
            time = indexingThread->lastIndexingTime;
        }
        sleep(data->t - difftime(currentTime, time));
    }
}

void countTypes(pthread_mutex_t *databaseMutex) {
    int jpgCount = 0, pngCount = 0, zipCount = 0, gzipCount = 0, folderCount = 0;

    pthread_mutex_lock(databaseMutex);

    for (int i = 0; i < global.mainDatabase.currentIdx; ++i) {
        if (strcmp(global.mainDatabase.database[i].fileType, "jpeg") == 0) {
            jpgCount++;
        } else if (strcmp(global.mainDatabase.database[i].fileType, "png") == 0) {
            pngCount++;
        } else if (strcmp(global.mainDatabase.database[i].fileType, "zip") == 0) {
            zipCount++;
        } else if (strcmp(global.mainDatabase.database[i].fileType, "gzip") == 0) {
            gzipCount++;
        } else
            folderCount++;
    }
    fprintf(stdout, "jpg Count: %d\n", jpgCount);
    fprintf(stdout, "png Count: %d\n", pngCount);
    fprintf(stdout, "zip Count: %d\n", zipCount);
    fprintf(stdout, "gzip Count: %d\n", gzipCount);
    fprintf(stdout, "folder Count: %d\n", folderCount);

    pthread_mutex_unlock(databaseMutex);
}

int compareSize(int i, int *size) {
    if (size == NULL) return 0;
    if (global.mainDatabase.database[i].size > *size) {
        return 1;
    }
    return 0;
}

int compareName(int i, char* name) {
    if (name == NULL) return 0;
    if (strstr(global.mainDatabase.database[i].fileName, name) != NULL) {
        return 1;
    }
    return 0;
}

int compareUID(int i, uid_t* UID) {
    if (UID == NULL) return 0;
    if (global.mainDatabase.database[i].UID == *UID) {
        return 1;
    }
    return 0;
}

void printCommand(FILE *stream, int type, int* size, char* name, uid_t* UID) {
    for (int i = 0; i < global.mainDatabase.currentIdx; i++) {
        if ((type == 1 && compareSize(i, size)) || (type == 2 && compareName(i, name)) ||
            (type == 3 && compareUID(i, UID))) {
            fprintf(stream, "%s %ld %s \n",
                    global.mainDatabase.database[i].path,
                    global.mainDatabase.database[i].size,
                    global.mainDatabase.database[i].fileType);
        }
    }
}

void executeCommand(int type, int* size, char* name, uid_t* UID, pthread_mutex_t *databaseMutex) {
    char *pager = getenv("PAGER");
    FILE *f;
    int lines = 0;

    pthread_mutex_lock(databaseMutex);
    for (int i = 0; i < global.mainDatabase.currentIdx; i++) {
        if ((type == 1 && compareSize(i, size)) || (type == 2 && compareName(i, name)) ||
            (type == 3 && compareUID(i, UID))) {
            lines++;
            if (lines > 3) break;
        }
    }

    if (lines > 3 && pager != NULL) {
        if ((f = popen(pager, "w")) == NULL) ERR("popen");
        printCommand(f, type, size, name, UID);
        pthread_mutex_unlock(databaseMutex);
        pclose(f);
    } else {
        printCommand(stdout, type, size, name, UID);
        pthread_mutex_unlock(databaseMutex);
    }
}

int main(int argc, char **argv) {
    // program init - arguments handling
    char *d = NULL;
    char *m = NULL;
    int t = 0;
    int mFlag = 0;
    readArguments(argc, argv, &d, &m, &t, &mFlag);

    // if == 0 there is no indexing running, if == 1 there is an indexing in progress
    int indexingFlag = 0;
    global.reindexingFlag = 0;

    // indexing thread structure
    pthread_mutex_t indexingFlagMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t databaseMutex = PTHREAD_MUTEX_INITIALIZER;
    threadData indexingThread = {.databaseMutex = &databaseMutex,
            .indexingFlagMutex = &indexingFlagMutex,
            .d = d,
            .m = m,
            .indexingFlag = &indexingFlag,
            .mFlag = &mFlag};

    // program init - open file or create it
    pthread_mutex_lock(&databaseMutex);
    openFile(m, &indexingThread);
    pthread_mutex_unlock(&databaseMutex);

    if (global.mainDatabase.fileDescriptor > 0) {
        printf("Index file successfully loaded! Awaiting instructions.\n");
    } else {
        printf("File doesn't exist! Creating new file and indexing in progress...\n");
        createFile(m, &indexingThread);
    }

    // program init - launching periodic indexing if param t is provided
    threadData periodicIndexer = {.indexingThread = &indexingThread,
            .indexingFlagMutex = &indexingFlagMutex,
            .indexingFlag = &indexingFlag,
            .t = t,
            .databaseMutex = &databaseMutex};
    if (t != 0) {
        int err = pthread_create(&periodicIndexer.threadID, NULL, periodicIndexing, &periodicIndexer);
        if (err != 0) ERR("pthread_create");
    }

    // commands loop
    char input[MAX_INPUT_LENGTH];
    while (1) {
        if (fgets(input, MAX_INPUT_LENGTH, stdin) == NULL) {
            shutdownProcedure(&indexingThread);
            return EXIT_FAILURE;
        }

        char *pos;
        if ((pos = strchr(input, '\n')) != NULL)
            *pos = '\0';

        if (strcmp(input, "exit!") == 0) {
            if (t != 0) {
                pthread_cancel(periodicIndexer.threadID);
            }
            pthread_cancel(indexingThread.threadID);
            return EXIT_SUCCESS;
        }

        if (strcmp(input, "exit") == 0) {
            pthread_mutex_lock(&indexingFlagMutex);
            if (indexingFlag == 1) {
                pthread_mutex_unlock(&indexingFlagMutex);
                printf("Indexing in progress. Please wait.\n");
                pthread_join(indexingThread.threadID, NULL);
            } else {
                pthread_mutex_unlock(&indexingFlagMutex);
            }
            shutdownProcedure(&indexingThread);
            return EXIT_SUCCESS;
        }

        if (strcmp(input, "index") == 0) {
            pthread_mutex_lock(&indexingFlagMutex);
            if (indexingFlag == 1) {
                pthread_mutex_unlock(&indexingFlagMutex);
                printf("Indexing already in progress, please wait!\n");
            } else {
                pthread_mutex_unlock(&indexingFlagMutex);
                time(&indexingThread.lastIndexingTime);
                int err = pthread_create(&indexingThread.threadID, NULL, reindexFiles, &indexingThread);
                if (err != 0) ERR("pthread_create");
            }
        }

        if (strcmp(input, "count") == 0) {
            countTypes(&databaseMutex);
        }

        if (strstr(input, "largerthan") != NULL) {
            char *p = strchr(input, ' ');
            int size = atoi(p + 1);
            executeCommand(1, &size, NULL, NULL, &databaseMutex);
        }

        if (strstr(input, "namepart") != NULL) {
            char *p = strchr(input, ' ');
            char *filename = p + 1;
            executeCommand(2, NULL, filename, NULL, &databaseMutex);
        }

        if (strstr(input, "owner") != NULL) {
            char *p = strchr(input, ' ');
            uid_t uid = atoi(p + 1);
            executeCommand(3, NULL, NULL, &uid, &databaseMutex);
        }
    }
}
