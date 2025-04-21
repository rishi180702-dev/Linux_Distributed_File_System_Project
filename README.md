# Linux_Distributed_File_System_Project

Project Overview
This repository implements a Distributed File System in C using UNIX socket programming. It consists of:

S1 (Main Server)

Listens for client connections on a well‑known port.

Forks a child process for each client to handle commands concurrently.

Stores all “.c” files locally under ~/S1.

Transparently forwards other file types to specialized back‑end servers:

.pdf → S2

.txt → S3

.zip → S4

Supports client commands:

uploadf <filename> <~S1/path>

downlf <~S1/path>

removef <~S1/path>

downltar <filetype>

dispfnames <~S1/directory> ​

S2, S3, S4 (Type‑Specific Servers)

Each listens on its own port and manages one file type in ~/S2, ~/S3, or ~/S4.

Implements commands from S1:

STORE <path> <size> to save files

GET <path> to retrieve files

DEL <path> to delete files

LIST <path> (and for S2/S3: TAR) to list or bundle files

w25clients (Client Application)

Connects only to S1, so clients remain unaware of the back‑end servers.

Parses user commands, sends them to S1, and handles file data transfers. ​

Key Features

Transparent Distribution: Clients use a single interface (S1), while the system routes files by type behind the scenes.

Concurrency: Uses fork() in S1 and multithreading in S2–S4 to serve multiple clients simultaneously.

Robust File Operations: Supports upload, download, delete, directory listing, and on‑demand archiving (tar).

Dynamic Directory Management: Automatically creates nested directories in each server’s home folder as needed. ​

Tech Stack & Tools

Language: C (POSIX sockets, fork(), pthread)

Build: gcc S1.c -o S1, gcc S2.c -o S2 -lpthread, etc.

Environment: UNIX/Linux (assumes ~/S1, ~/S2, ~/S3, ~/S4 exist)

Getting Started

Prepare directories under your home:

bash
Copy
Edit
mkdir -p ~/S1 ~/S2 ~/S3 ~/S4
Compile servers & client:

bash
Copy
Edit
gcc S1.c -o S1
gcc S2.c -o S2 -lpthread
gcc S3.c -o S3 -lpthread
gcc S4.c -o S4 -lpthread
gcc w25clients.c -o w25clients
Run each server in its own terminal:

bash
Copy
Edit
./S1    # listens on port 50004
./S2    # listens on port 50005
./S3    # listens on port 50006
./S4    # listens on port 50007
Start the client and issue commands:

bash
Copy
Edit
./w25clients 127.0.0.1 50004
w25clients$ uploadf sample.c ~S1/projects
w25clients$ downlf ~S1/projects/sample.c
This design exercise demonstrates core systems programming skills—socket communication, process/thread management, file I/O, and directory handling—by building a simple, type‑aware distributed file storage service.
