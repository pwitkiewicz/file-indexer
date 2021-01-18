# POSIX compliant file-indexer

The aim of the project was to create a program that traverses all files in a given directory and its subdirectories, 
creates a data structure containing the requested information about the files and then waits for user input. 
User inputs commands that query data gathered in the data structure. To avoid repetitive scanning of the directory,
the data structure is written to a file and read when the program is run again.

## Arguments
**-d path**

a path to a directory that will be traversed, if the option is not present a path set in an environment 
variable `$MOLE_DIR` is used. If the environment variable is not set the program end with an error. 

**-f path**

a path to a file where index is stored. If the option is not present, the value from environment 
variable `$MOLE_INDEX_PATH` is used. If the variable is not set, the default value of file `.mole-index`
in user's home directory is used.

**-t n**

where n is an integer from the range [30,7200]. n denotes a time between subsequent rebuilds of index.
This parameter is optional. If it is not present, the periodic re-indexing is disabled. 

## Program specification
When stated, the program tries to open a file pointed by `path f` and if the file exists index from 
the file is read otherwise the program starts indexing procedure described later. After that program
starts waiting for user's input on stdin.

### Indexing procedure
Index stores the information about the following file types:
+ directories
+ JPEG images
+ PNG images
+ gzip compressed files
+ zip compressed files (including any files based on zip format like docx, odt, …).

A file type recognition is be based on a file signature (a so called magic number) not a file name extension. 
Any file types other than the above are excluded from index. Index stores the following information about each file:
+ file name
+ a full (absolute) path to a file
+ size
+ owner's uid
+ type (one of the above).

The indexing procedure works as follows: a single thread is started. The thread creates a new index by traversing all files in 
`path d` and its subdirectories. For each file a file type is checked and if the type is one of the indexed types, the required 
data is stored in index. Once traversal is complete, the index structure is written to `path f`.

### Available commands
A command processing works parallel to a potential re-indexing process. The commands is processed even if indexing is in
progress. As the new index structure is not ready an old version may be used to provide user with the answers.
The program reads subsequent lines from stdin. Each line should contain the one of the following commands. 
If the read line is not a command an error message is printed and the program waits for the next line.

Commands:
+ `exit` – starts a termination procedure, the program stops reading commands from stdin. If an indexing is currently in progress, the program waits for it to finish.
+ `exit!` – quick termination, the program stops reading commands from stdin. If any indexing is in progress it is canceled. 
+ `index` – if there is no currently running indexing operation a new indexing is started in background and the program immediately starts waiting for the next command. If there is currently running indexing operation a warining message is printed and no additional tasks are performed.
+ `count` – calculates the counts of each file type in index and prints them to stdout.
+ `largerthan x` – x is the requested file size. Prints full path, size and type of all files in index that have size larger than x.
+ `namepart y` – y is a part of a filename, it may contain spaces. Prints the same information as previous command about all files that contain y in the name.
+ `owner uid` – uid is owner's identifier. Same as the previous one but prints information about all files that owner is uid.

### Reindexing
If the parameter `t` s present, the program starts a thread that runs indexing process when the index is older than `t` seconds. A time is counted from either last re-indexing on timeout or a manual re-index whichever is later. If the index was read from a file the last indexing time is set to the file modification time (this may trigger an immediate re-indexing after reading an old file).
