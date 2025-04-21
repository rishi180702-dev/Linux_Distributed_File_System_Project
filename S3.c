/*****************************************************************************
 * S3.c
 *
 * This server handles all .txt file operations on behalf of S1. It should run
 * in its own terminal, listening on port 9003 by default. Whenever S1 needs to
 * store, retrieve, delete, or list .txt files, it connects here.
 *
 * Behavior:
 *  - Files are physically stored under ~/S3.
 *  - The following commands are recognized (all sent by S1):
 *
 *    1) STORE <path> <size>
 *       - S3 receives <size> bytes from the socket and writes them to
 *         ~/S3/<path>.
 *       - On success, respond "SUCCESS\n"; on failure, "ERROR\n".
 *
 *    2) GET <path>
 *       - S3 looks up ~/S3/<path>. If found, sends back:
 *         "<filesize>\n<filedata>"
 *       - Otherwise: "ERROR: File not found\n"
 *
 *    3) DEL <path>
 *       - Removes the file at ~/S3/<path>. On success, "SUCCESS\n"; else
 *         "ERROR\n".
 *
 *    4) TAR
 *       - Creates a tar of all files under ~/S3, named /tmp/text.tar.
 *         Then responds with "<tarsize>\n<tarfile>".
 *       - If anything fails, respond "ERROR\n".
 *
 *    5) LIST <path>
 *       - Lists all regular (non-hidden) files in ~/S3/<path>, returning
 *         "<size>\n<list>" where <list> is newline-separated filenames.
 *       - If directory doesn't exist or empty, can send "0\n".
 *
 * Build (on Linux/Unix):
 *     gcc S3.c -o S3 -lpthread
 *
 * Usage:
 *     ./S3
 *
 * By default, it listens on 127.0.0.1:9003. If you want a different address or
 * port, edit the #defines below.
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
 
 #define S3_PORT 50006
 #define BACKLOG 10
 #define BUF_SIZE 4096
 
 // Simple logging macro for S3 server messages
 #define LOG(msg, ...) fprintf(stderr, "S3: " msg "\n", ##__VA_ARGS__)
 
 /*****************************************************************************
  * Thread function: handles commands from a single S1 connection.
  *****************************************************************************/
 void *handle_client(void *arg) {
     int clientSock = *(int*)arg;
     free(arg);  // dynamically allocated in the accept loop
     char buffer[1024];
 
     // Continuously read commands from S1 until the connection is closed
     while (1) {
         // Read one line (until newline)
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
                 break; // end of command
             }
             if (idx < (int)sizeof(buffer) - 1) {
                 buffer[idx++] = c;
             }
         }
         buffer[idx] = '\0';
         if (idx == 0) {
             // Empty command => ignore
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
             // Format: STORE <path> <size>
             char *path = strtok(NULL, " ");
             char *sizeStr = strtok(NULL, " ");
             if (!path || !sizeStr) {
                 const char *err = "ERROR: Invalid STORE command\n";
                 send(clientSock, err, strlen(err), 0);
                 continue;
             }
             long fileSize = atol(sizeStr);
 
             // Build full path under ~/S3
             char *home = getenv("HOME");
             if (!home) {
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
                 continue;
             }
             // Base directory
             char baseDir[512];
             snprintf(baseDir, sizeof(baseDir), "%s/S3", home);
 
             // If S1 included ~S3 in the path, remove it
             const char *relPath = path;
             if (strncmp(path, "~S3", 3) == 0) {
                 relPath = path + 3;
                 if (*relPath == '/') {
                     relPath++;
                 }
             }
 
             // Final path
             char fullPath[1024];
             snprintf(fullPath, sizeof(fullPath), "%s/%s", baseDir, relPath);
 
             // Ensure subdirectories exist
             {
                 char dirPath[1024];
                 strncpy(dirPath, fullPath, sizeof(dirPath));
                 dirPath[sizeof(dirPath)-1] = '\0';
                 char *lastSlash = strrchr(dirPath, '/');
                 if (lastSlash) {
                     *lastSlash = '\0';
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
 
             // Open file for writing
             FILE *fp = fopen(fullPath, "wb");
             if (!fp) {
                 LOG("Failed to open %s: %s", fullPath, strerror(errno));
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
 
                 // Drain incoming data to sync
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
 
             // Read the file content from S1
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
                 // Connection lost mid-transfer
                 LOG("Connection lost during STORE of %s", fullPath);
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
 
             // Build full path under ~/S3
             char baseDir[512];
             snprintf(baseDir, sizeof(baseDir), "%s/S3", home);
             const char *relPath = path;
             if (strncmp(path, "~S3", 3) == 0) {
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
 
             fseek(fp, 0, SEEK_END);
             long fileSize = ftell(fp);
             fseek(fp, 0, SEEK_SET);
             char sizeStr[64];
             snprintf(sizeStr, sizeof(sizeStr), "%ld\n", fileSize);
             send(clientSock, sizeStr, strlen(sizeStr), 0);
 
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
             snprintf(baseDir, sizeof(baseDir), "%s/S3", home);
             const char *relPath = path;
             if (strncmp(path, "~S3", 3) == 0) {
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
            // Extract file type from the command (we're expecting "TAR .txt")
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
            
            // Ensure we're processing the correct file type for S3
            if (strlen(fileType) == 0 || strcmp(fileType, ".txt") != 0) {
                const char *err = "ERROR: S3 only handles .txt files\n";
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
            char tempFile[] = "/tmp/txtXXXXXX";
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
                    "find %s/S3 -name \"*%s\" -type f | xargs tar -cf %s 2>/dev/null",
                    home, fileType, tempFile);
            
            LOG("Executing tar command: %s", tarCmd);
            
            // Attempt to create the tar
            int status = system(tarCmd);
            if (status != 0) {
                LOG("tar command failed with status %d", status);
                
                // Check if there are any matching files
                char checkCmd[512];
                snprintf(checkCmd, sizeof(checkCmd), 
                        "find %s/S3 -name \"*%s\" -type f | wc -l", 
                        home, fileType);
                
                FILE *checkP = popen(checkCmd, "r");
                if (!checkP) {
                    const char *err = "ERROR: Failed to check for txt files\n";
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
                    LOG("Created empty tar file (no txt files found)");
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
            LOG("Sent txt.tar (%ld bytes) to S1", tarSize);
        }
 
         /*********************************************************************
          * 5) LIST <path>
          *********************************************************************/
         else if (strcmp(cmd, "LIST") == 0) {
             char *path = strtok(NULL, "");
             if (path && *path == ' ') {
                 path++;
             }
             if (!path || strlen(path) == 0 || strcmp(path, ".") == 0) {
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
                 snprintf(dirPath, sizeof(dirPath), "%s/S3", home);
             } else {
                 if (strncmp(path, "~S3", 3) == 0) {
                     path += 3;
                     if (*path == '/') {
                         path++;
                     }
                 }
                 snprintf(dirPath, sizeof(dirPath), "%s/S3/%s", home, path);
             }
             DIR *dp = opendir(dirPath);
             if (!dp) {
                 const char *err = "0\n";
                 send(clientSock, err, strlen(err), 0);
             } else {
                 char listBuf[2048];
                 listBuf[0] = '\0';
                 struct dirent *entry;
                 while ((entry = readdir(dp)) != NULL) {
                     if (entry->d_name[0] == '.') {
                         continue;  // skip hidden files
                     }
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
          * Unknown command
          *********************************************************************/
         } else {
             const char *err = "ERROR: Unknown command\n";
             send(clientSock, err, strlen(err), 0);
         }
     }
 
     close(clientSock);
     return NULL;
 }
 
 /*****************************************************************************
  * main: Sets up a listening socket on port 9003. For each connection from S1,
  * we spawn a new thread to handle commands. Typically, S1 uses S3 for .txt
  * files (upload, download, remove, tar, list).
  *****************************************************************************/
 int main() {
     // Create a socket
     int servSock = socket(AF_INET, SOCK_STREAM, 0);
     if (servSock < 0) {
         perror("socket");
         exit(EXIT_FAILURE);
     }
 
     // Reuse port
     int opt = 1;
     setsockopt(servSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
 
     // Bind to local port 9003
     struct sockaddr_in servaddr;
     memset(&servaddr, 0, sizeof(servaddr));
     servaddr.sin_family      = AF_INET;
     servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
     servaddr.sin_port        = htons(S3_PORT);
 
     if (bind(servSock, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
         perror("bind");
         close(servSock);
         exit(EXIT_FAILURE);
     }
 
     // Listen
     if (listen(servSock, BACKLOG) < 0) {
         perror("listen");
         close(servSock);
         exit(EXIT_FAILURE);
     }
     LOG("Server listening on port %d", S3_PORT);
 
     // Accept loop: spawn a thread for each connection
     while (1) {
         struct sockaddr_in cliaddr;
         socklen_t clilen = sizeof(cliaddr);
         int *clientSock = malloc(sizeof(int)); // Freed by the thread
         if (!clientSock) {
             continue; // out of memory?
         }
         *clientSock = accept(servSock, (struct sockaddr*)&cliaddr, &clilen);
         if (*clientSock < 0) {
             free(clientSock);
             if (errno == EINTR) {
                 continue;
             }
             perror("accept");
             break;
         }
 
         pthread_t tid;
         if (pthread_create(&tid, NULL, handle_client, clientSock) != 0) {
             perror("pthread_create");
             close(*clientSock);
             free(clientSock);
         } else {
             pthread_detach(tid); // automatically reclaim resources
         }
     }
 
     close(servSock);
     return 0;
 }
 