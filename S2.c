/*****************************************************************************
 * S2.c
 *
 * This server handles all .pdf file operations on behalf of S1. It must be run
 * in a separate terminal/process, listening on port 9002 by default. S1 will
 * connect here whenever it needs to store, retrieve, delete, or list PDF files.
 *
 * Behavior:
 *  - Files are physically stored under ~/S2, in a directory structure matching
 *    what S1 requested (e.g. ~/S2/folder1/folder2/...).
 *  - The following commands are recognized, all sent by S1:
 *
 *    1) STORE <path> <size>
 *       - S2 receives <size> bytes from the socket and writes them to
 *         ~/S2/<path>.
 *       - On success, respond with "SUCCESS\n".
 *       - On failure, respond with "ERROR\n".
 *
 *    2) GET <path>
 *       - S2 looks up ~/S2/<path>. If found, sends back:
 *         "<filesize>\n<filedata>"
 *       - If not found, sends: "ERROR: File not found\n"
 *
 *    3) DEL <path>
 *       - S2 removes the file at ~/S2/<path> if it exists.
 *       - On success, respond with "SUCCESS\n".
 *       - On failure, respond with "ERROR\n".
 *
 *    4) TAR
 *       - S2 tars all files under ~/S2 (pdf or otherwise, though the spec
 *         focuses on pdf). Then sends:
 *         "<tarsize>\n<tarfile>"
 *       - If cannot create the tar, respond with "ERROR\n".
 *
 *    5) LIST <path>
 *       - S2 lists the files (by name only) in the directory ~/S2/<path> if it
 *         exists, ignoring hidden files. The format is:
 *         "<size>\n<listdata>"
 *       - If directory doesn't exist or is empty, it may send "0\n".
 *
 * Build (on Linux/Unix):
 *     gcc S2.c -o S2 -lpthread
 *
 * Usage:
 *     ./S2
 *
 * By default, it listens on 127.0.0.1:9002. If you want a different port or IP
 * address, edit the #defines accordingly.
 *****************************************************************************/

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <errno.h>
 #include <pthread.h>
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
 #include <sys/stat.h>
 #include <dirent.h>
 
 #define S2_PORT 50005
 #define BACKLOG 10
 #define BUF_SIZE 4096
 
 // Simple logging macro. Writes to stderr with a "S2:" prefix.
 #define LOG(msg, ...) fprintf(stderr, "S2: " msg "\n", ##__VA_ARGS__)
 
 /*****************************************************************************
  * "Thread routine" to handle commands from a single S1 connection.
  * We'll read one command at a time, process it, and loop until S1 disconnects.
  *****************************************************************************/
 void *handle_client(void *arg) {
     int clientSock = *(int*)arg;
     free(arg);  // arg was dynamically allocated in the accept loop
     char buffer[1024];
 
     while (1) {
         // Read a full line (command) until newline
         int idx = 0;
         char c;
         while (1) {
             ssize_t n = recv(clientSock, &c, 1, 0);
             if (n <= 0) {
                 // Connection closed or error
                 close(clientSock);
                 return NULL;
             }
             if (c == '\n') {
                 break;  // end of command
             }
             if (idx < (int)sizeof(buffer) - 1) {
                 buffer[idx++] = c;
             }
         }
         buffer[idx] = '\0';
         if (idx == 0) {
             // Empty command -> ignore
             continue;
         }
 
         // Parse the command
         char *cmd = strtok(buffer, " ");
         if (!cmd) {
             continue;
         }
 
         /*********************************************************************
          * 1) STORE <path> <size>
          *********************************************************************/
         if (strcmp(cmd, "STORE") == 0) {
             char *path = strtok(NULL, " ");
             char *sizeStr = strtok(NULL, " ");
             if (!path || !sizeStr) {
                 const char *err = "ERROR: Invalid STORE command\n";
                 send(clientSock, err, strlen(err), 0);
                 continue;
             }
             long fileSize = atol(sizeStr);
 
             // Build the full path under ~/S2
             char *home = getenv("HOME");
             if (!home) {
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
                 continue;
             }
             // Base directory
             char baseDir[512];
             snprintf(baseDir, sizeof(baseDir), "%s/S2", home);
 
             // If S1 included something like "~S2/..."
             // we strip off "~S2" from path and keep the remainder
             const char *relPath = path;
             if (strncmp(path, "~S2", 3) == 0) {
                 relPath = path + 3;
                 if (*relPath == '/') {
                     relPath++;
                 }
             }
 
             // Construct the final path
             char fullPath[1024];
             snprintf(fullPath, sizeof(fullPath), "%s/%s", baseDir, relPath);
 
             // Ensure subdirectories exist
             // (We do a manual approach here by chopping off the filename at the last slash)
             {
                 char dirPath[1024];
                 strncpy(dirPath, fullPath, sizeof(dirPath));
                 dirPath[sizeof(dirPath)-1] = '\0';
                 char *lastSlash = strrchr(dirPath, '/');
                 if (lastSlash) {
                     *lastSlash = '\0';
                     // Make subdirs as needed
                     char tmp[1024];
                     snprintf(tmp, sizeof(tmp), "%s", dirPath);
                     for (char *p = tmp + 1; *p; ++p) {
                         if (*p == '/') {
                             *p = '\0';
                             mkdir(tmp, 0755);
                             *p = '/';
                         }
                     }
                     mkdir(tmp, 0755);
                 }
             }
 
             // Open the file for writing
             FILE *fp = fopen(fullPath, "wb");
             if (!fp) {
                 LOG("Failed to open %s: %s", fullPath, strerror(errno));
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
                 // Drain incoming data (because S1 will still send fileSize bytes)
                 char discard[512];
                 long remaining = fileSize;
                 while (remaining > 0) {
                     ssize_t r = recv(clientSock, discard,
                                      remaining < (long)sizeof(discard) ? remaining : sizeof(discard),
                                      0);
                     if (r <= 0) {
                         break;
                     }
                     remaining -= r;
                 }
                 continue;
             }
 
             // Receive file data
             long remaining = fileSize;
             char dataBuf[BUF_SIZE];
             while (remaining > 0) {
                 ssize_t r = recv(clientSock, dataBuf,
                                  remaining < (long)sizeof(dataBuf) ? remaining : sizeof(dataBuf),
                                  0);
                 if (r <= 0) {
                     break;
                 }
                 fwrite(dataBuf, 1, r, fp);
                 remaining -= r;
             }
             fclose(fp);
 
             if (remaining != 0) {
                 // Connection lost in the middle of file
                 LOG("Connection lost while storing %s", fullPath);
                 remove(fullPath);
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
             } else {
                 LOG("Stored file %s (%ld bytes)", fullPath, fileSize);
                 const char *succ = "SUCCESS\n";
                 send(clientSock, succ, strlen(succ), 0);
             }
 
         /*********************************************************************
          * 2) GET <path>
          *********************************************************************/
         } else if (strcmp(cmd, "GET") == 0) {
             // Command format: GET <path>
             char *path = strtok(NULL, "");
             if (!path) {
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
                 continue;
             }
             // Trim spaces
             while (*path == ' ') {
                 path++;
             }
             char *home = getenv("HOME");
             if (!home) {
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
                 continue;
             }
 
             // Build full path under ~/S2
             char baseDir[512];
             snprintf(baseDir, sizeof(baseDir), "%s/S2", home);
             const char *relPath = path;
             if (strncmp(path, "~S2", 3) == 0) {
                 relPath = path + 3;
                 if (*relPath == '/') {
                     relPath++;
                 }
             }
             char fullPath[1024];
             snprintf(fullPath, sizeof(fullPath), "%s/%s", baseDir,
                      (*relPath ? relPath : "."));
 
             FILE *fp = fopen(fullPath, "rb");
             if (!fp) {
                 const char *err = "ERROR: File not found\n";
                 send(clientSock, err, strlen(err), 0);
                 continue;
             }
             // Send file size
             fseek(fp, 0, SEEK_END);
             long fileSize = ftell(fp);
             fseek(fp, 0, SEEK_SET);
             char sizeStr[64];
             snprintf(sizeStr, sizeof(sizeStr), "%ld\n", fileSize);
             send(clientSock, sizeStr, strlen(sizeStr), 0);
 
             // Send file data
             char sendBuf[BUF_SIZE];
             size_t bytes;
             while ((bytes = fread(sendBuf, 1, sizeof(sendBuf), fp)) > 0) {
                 send(clientSock, sendBuf, bytes, 0);
             }
             fclose(fp);
             LOG("Sent file %s (%ld bytes)", fullPath, fileSize);
 
         /*********************************************************************
          * 3) DEL <path>
          *********************************************************************/
         } else if (strcmp(cmd, "DEL") == 0) {
             // DEL <path>
             char *path = strtok(NULL, "");
             if (!path) {
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
                 continue;
             }
             while (*path == ' ') {
                 path++;
             }
             char *home = getenv("HOME");
             if (!home) {
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
                 continue;
             }
             char baseDir[512];
             snprintf(baseDir, sizeof(baseDir), "%s/S2", home);
             const char *relPath = path;
             if (strncmp(path, "~S2", 3) == 0) {
                 relPath = path + 3;
                 if (*relPath == '/') {
                     relPath++;
                 }
             }
             char fullPath[1024];
             snprintf(fullPath, sizeof(fullPath), "%s/%s", baseDir,
                      (*relPath ? relPath : ""));
 
             if (unlink(fullPath) == 0) {
                 LOG("Deleted file %s", fullPath);
                 const char *succ = "SUCCESS\n";
                 send(clientSock, succ, strlen(succ), 0);
             } else {
                 LOG("Failed to delete %s: %s", fullPath, strerror(errno));
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
             }

             
        // Now remove empty parent directories upward (like what you did for S1)
        char currentDir[512];
        strncpy(currentDir, fullPath, sizeof(currentDir));
        // Remove the file name from currentDir to get the containing directory.
        char *lastSlash = strrchr(currentDir, '/');
        if (lastSlash != NULL) {
            *lastSlash = '\0';
        } else {
            // Should not happen, but bail out if we can't determine a directory
            LOG("Failed to determine parent directory for %s", fullPath);
            const char *succ = "SUCCESS\n";
            send(clientSock, succ, strlen(succ), 0);
            continue;
        }

        // Loop to remove directories until reaching the baseDir.
        // This will remove empty subdirectories like ~S2/folder1/folder2, etc.
        while (strcmp(currentDir, baseDir) != 0) {
            if (rmdir(currentDir) == 0) {
                LOG("Removed empty directory: %s", currentDir);
                // Move up one level: strip the last component
                lastSlash = strrchr(currentDir, '/');
                if (lastSlash != NULL) {
                    *lastSlash = '\0';
                } else {
                    break; // No more slashes, stop the cleanup loop.
                }
            } else {
                // If a directory is not empty or removal fails, stop attempting further removal.
                LOG("Directory %s not empty or could not be removed (errno: %d)", currentDir, errno);
                break;
            }
        }
 
         /*********************************************************************
          * 4) TAR
          *********************************************************************/
         }else if (strncmp(cmd, "TAR", 3) == 0) {
            // Extract file type from the command (we're expecting "TAR .pdf")
            char fileType[10] = {0};
            
            // Extract everything after "TAR" and trim whitespace
            char *typePtr = cmd + 3;
            while (*typePtr && (*typePtr == ' ' || *typePtr == '\t'))
                typePtr++;
                
            // Copy the file type
            if (*typePtr) {
                sscanf(typePtr, "%9s", fileType);
                LOG("Received TAR command for file type: '%s'", fileType);
            }
            
            // Ensure we're processing the correct file type for S2
            if (strlen(fileType) == 0 || strcmp(fileType, ".pdf") != 0) {
                const char *err = "ERROR: S2 only handles .pdf files\n";
                send(clientSock, err, strlen(err), 0);
                continue;
            }
            
            // Get HOME directory
            char *home = getenv("HOME");
            if (!home) {
                const char *err = "ERROR: HOME environment variable not set\n";
                send(clientSock, err, strlen(err), 0);
                continue;
            }
            
            // Create a temporary file for the tar
            char tempFile[] = "/tmp/pdfXXXXXX";
            int temp_fd = mkstemp(tempFile);
            if (temp_fd == -1) {
                const char *err = "ERROR: Failed to create temporary file\n";
                send(clientSock, err, strlen(err), 0);
                continue;
            }
            close(temp_fd); // Close the fd; tar will create/overwrite the file
            
            // Build the tar command - find all .pdf files in the S2 directory tree
            char tarCmd[1024];
            snprintf(tarCmd, sizeof(tarCmd), 
                    "find %s/S2 -name \"*%s\" -type f | xargs tar -cf %s 2>/dev/null",
                    home, fileType, tempFile);
            
            LOG("Executing tar command: %s", tarCmd);
            
            // Attempt to create the tar
            int status = system(tarCmd);
            if (status != 0) {
                LOG("tar command failed with status %d", status);
                
                // Check if there are any matching files
                char checkCmd[512];
                snprintf(checkCmd, sizeof(checkCmd), 
                        "find %s/S2 -name \"*%s\" -type f | wc -l", 
                        home, fileType);
                
                FILE *checkP = popen(checkCmd, "r");
                if (!checkP) {
                    const char *err = "ERROR: Failed to check for PDF files\n";
                    send(clientSock, err, strlen(err), 0);
                    remove(tempFile);
                    continue;
                }
                
                char countStr[32] = {0};
                fgets(countStr, sizeof(countStr), checkP);
                pclose(checkP);
                int count = atoi(countStr);
                
                if (count == 0) {
                    // No files found - create an empty tar
                    FILE *emptyTar = fopen(tempFile, "wb");
                    if (!emptyTar) {
                        const char *err = "ERROR: Failed to create empty tar\n";
                        send(clientSock, err, strlen(err), 0);
                        remove(tempFile);
                        continue;
                    }
                    fclose(emptyTar);
                    LOG("Created empty tar file (no PDF files found)");
                } else {
                    // Try alternative approach
                    snprintf(tarCmd, sizeof(tarCmd), 
                            "tar -cf %s --no-recursion -C $(find %s/S2 -name \"*%s\" -type f -printf \"%%P\\n\") 2>/dev/null", 
                            tempFile, home, home, fileType);
                    
                    LOG("Trying alternative tar command: %s", tarCmd);
                    status = system(tarCmd);
                    
                    if (status != 0) {
                        const char *err = "ERROR: Failed to create tar file\n";
                        send(clientSock, err, strlen(err), 0);
                        remove(tempFile);
                        continue;
                    }
                }
            }
            
            // Send the tar to S1
            FILE *fp = fopen(tempFile, "rb");
            if (!fp) {
                const char *err = "ERROR: Failed to open tar file\n";
                send(clientSock, err, strlen(err), 0);
                remove(tempFile);
                continue;
            }
            
            fseek(fp, 0, SEEK_END);
            long tarSize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            
            // First send the size
            char sizeStr[64];
            snprintf(sizeStr, sizeof(sizeStr), "%ld\n", tarSize);
            send(clientSock, sizeStr, strlen(sizeStr), 0);
            
            // Then the file itself
            char buf[BUF_SIZE];
            size_t bytes;
            while ((bytes = fread(buf, 1, sizeof(buf), fp)) > 0) {
                send(clientSock, buf, bytes, 0);
            }
            fclose(fp);
            
            // Remove the temporary tar file
            remove(tempFile);
            LOG("Sent pdf.tar (%ld bytes) to S1", tarSize);
        }
         /*********************************************************************
          * 5) LIST <path>
          *********************************************************************/
         else if (strcmp(cmd, "LIST") == 0) {
             // LIST <subdir>
             // We gather filenames from ~/S2/<subdir> (non-hidden regular files),
             // then send them as a newline-separated list, preceded by length.
             char *path = strtok(NULL, "");
             if (path && *path == ' ') {
                 path++;
             }
             if (!path || strlen(path) == 0 || strcmp(path, ".") == 0) {
                 // Means S1 wants the root of ~/S2
                 path = ".";
             }
 
             char *home = getenv("HOME");
             if (!home) {
                 const char *err = "0\n";
                 send(clientSock, err, strlen(err), 0);
                 continue;
             }
             char dirPath[1024];
             if (strcmp(path, ".") == 0) {
                 snprintf(dirPath, sizeof(dirPath), "%s/S2", home);
             } else {
                 if (strncmp(path, "~S2", 3) == 0) {
                     path += 3;
                     if (*path == '/') {
                         path++;
                     }
                 }
                 snprintf(dirPath, sizeof(dirPath), "%s/S2/%s", home, path);
             }
 
             // Try to open the directory
             DIR *dp = opendir(dirPath);
             if (!dp) {
                 // If it doesn't exist or can't open, return "0\n"
                 const char *err = "0\n";
                 send(clientSock, err, strlen(err), 0);
             } else {
                 char listBuf[2048];
                 listBuf[0] = '\0';
                 struct dirent *entry;
                 while ((entry = readdir(dp)) != NULL) {
                     // Skip hidden files/directories
                     if (entry->d_name[0] == '.') {
                         continue;
                     }
                     // Only add regular files
                     if (entry->d_type == DT_REG) {
                         strncat(listBuf, entry->d_name,
                                 sizeof(listBuf) - strlen(listBuf) - 1);
                         strncat(listBuf, "\n",
                                 sizeof(listBuf) - strlen(listBuf) - 1);
                     }
                 }
                 closedir(dp);
 
                 long len = (long)strlen(listBuf);
                 char lenStr[64];
                 snprintf(lenStr, sizeof(lenStr), "%ld\n", len);
                 send(clientSock, lenStr, strlen(lenStr), 0);
                 if (len > 0) {
                     send(clientSock, listBuf, len, 0);
                 }
             }
 
         /*********************************************************************
          * Unknown or unsupported command
          *********************************************************************/
         } else {
             const char *err = "ERROR: Unknown command\n";
             send(clientSock, err, strlen(err), 0);
         }
     } // end while(1)
 
     // If we ever exit the loop, we close the client socket
     close(clientSock);
     return NULL;
 }
 
 /*****************************************************************************
  * main: Sets up a listening socket on port 9002 and spawns a thread to handle
  * each connection. S1 is expected to connect on this port to store and retrieve
  * .pdf files.
  *****************************************************************************/
 int main() {
     // Create a socket
     int servSock = socket(AF_INET, SOCK_STREAM, 0);
     if (servSock < 0) {
         perror("socket");
         exit(EXIT_FAILURE);
     }
 
     // Allow reuse of the port
     int opt = 1;
     setsockopt(servSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
 
     // Bind to local port 9002
     struct sockaddr_in servaddr;
     memset(&servaddr, 0, sizeof(servaddr));
     servaddr.sin_family      = AF_INET;
     servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
     servaddr.sin_port        = htons(S2_PORT);
 
     if (bind(servSock, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
         perror("bind");
         close(servSock);
         exit(EXIT_FAILURE);
     }
 
     // Start listening
     if (listen(servSock, BACKLOG) < 0) {
         perror("listen");
         close(servSock);
         exit(EXIT_FAILURE);
     }
     LOG("Server listening on port %d", S2_PORT);
 
     // Accept loop: for each connection from S1, create a thread
     while (1) {
         struct sockaddr_in cliaddr;
         socklen_t clilen = sizeof(cliaddr);
         int *clientSock = malloc(sizeof(int));  // Freed in thread
         if (!clientSock) {
             continue; // not enough memory, skip
         }
         *clientSock = accept(servSock, (struct sockaddr*)&cliaddr, &clilen);
         if (*clientSock < 0) {
             free(clientSock);
             if (errno == EINTR) {
                 continue; // interrupted by signal, try again
             }
             perror("accept");
             break; // exit the loop
         }
 
         // Spawn a thread to handle commands from this S1 connection
         pthread_t tid;
         if (pthread_create(&tid, NULL, handle_client, clientSock) != 0) {
             perror("pthread_create");
             close(*clientSock);
             free(clientSock);
         } else {
             // Detach the thread so it cleans up automatically
             pthread_detach(tid);
         }
     }
 
     close(servSock);
     return 0;
 }
 