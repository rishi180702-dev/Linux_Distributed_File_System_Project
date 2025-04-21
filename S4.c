/*****************************************************************************
 * S4.c
 *
 * This server handles all .zip file operations on behalf of S1. It should run
 * in its own terminal, listening on port 9004 by default. Whenever S1 needs to
 * store, retrieve, delete, or list .zip files, it connects here.
 *
 * Behavior:
 *  - Files are physically stored under ~/S4.
 *  - The following commands are recognized (all sent by S1):
 *
 *    1) STORE <path> <size>
 *       - S4 receives <size> bytes and writes them to ~/S4/<path>.
 *       - On success, respond "SUCCESS\n"; on failure, "ERROR\n".
 *
 *    2) GET <path>
 *       - S4 looks up ~/S4/<path>. If found, sends back:
 *         "<filesize>\n<filedata>"
 *       - Otherwise: "ERROR: File not found\n"
 *
 *    3) DEL <path>
 *       - Removes the file at ~/S4/<path>. On success, "SUCCESS\n"; else
 *         "ERROR\n".
 * 
 *    5) LIST <path>
 *       - Lists all regular, non-hidden files under ~/S4/<path>. Responds with
 *         "<size>\n<filenames>".
 *       - If directory doesn't exist or is empty, "0\n".
 *
 * Build (on Linux/Unix):
 *     gcc S4.c -o S4 -lpthread
 *
 * Usage:
 *     ./S4
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
 
 #define S4_PORT 50007
 #define BACKLOG 10
 #define BUF_SIZE 4096
 
 // Simple logging macro for S4 messages
 #define LOG(msg, ...) fprintf(stderr, "S4: " msg "\n", ##__VA_ARGS__)
 
 /*****************************************************************************
  * Thread routine: handles commands from a single S1 connection until S1
  * disconnects.
  *****************************************************************************/
 void *handle_client(void *arg) {
     int clientSock = *(int*)arg;
     free(arg);  // allocated in the accept loop
     char buffer[1024];
 
     // Command loop
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
             // empty command => ignore
             continue;
         }
 
         // Parse command
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
 
             // Build full path under ~/S4
             char *home = getenv("HOME");
             if (!home) {
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
                 continue;
             }
             char baseDir[512];
             snprintf(baseDir, sizeof(baseDir), "%s/S4", home);
 
             // If path starts with ~S4, remove that prefix
             const char *relPath = path;
             if (strncmp(path, "~S4", 3) == 0) {
                 relPath = path + 3;
                 if (*relPath == '/') {
                     relPath++;
                 }
             }
             char fullPath[1024];
             snprintf(fullPath, sizeof(fullPath), "%s/%s", baseDir, relPath);
 
             // Make subdirectories
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
                 LOG("Open failed for %s: %s", fullPath, strerror(errno));
                 const char *err = "ERROR\n";
                 send(clientSock, err, strlen(err), 0);
                 // Drain data
                 char discard[512];
                 long remaining = fileSize;
                 while (remaining > 0) {
                     ssize_t r = recv(clientSock, discard,
                                      remaining < (long)sizeof(discard) ? remaining : sizeof(discard),
                                      0);
                     if (r <= 0) break;
                     remaining -= r;
                 }
                 continue;
             }
 
             // Receive file data from S1
             long remaining = fileSize;
             char dataBuf[BUF_SIZE];
             while (remaining > 0) {
                 ssize_t r = recv(clientSock, dataBuf,
                                  remaining < (long)sizeof(dataBuf) ? remaining : sizeof(dataBuf),
                                  0);
                 if (r <= 0) break;
                 fwrite(dataBuf, 1, r, fp);
                 remaining -= r;
             }
             fclose(fp);
 
             if (remaining != 0) {
                 LOG("Lost connection while storing %s", fullPath);
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
             // GET <path>
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
             snprintf(baseDir, sizeof(baseDir), "%s/S4", home);
             const char *relPath = path;
             if (strncmp(path, "~S4", 3) == 0) {
                 relPath = path + 3;
                 if (*relPath == '/') {
                     relPath++;
                 }
             }
             char fullPath[1024];
             snprintf(fullPath, sizeof(fullPath), "%s/%s",
                      baseDir, (*relPath ? relPath : "."));
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
          * 5) LIST <path>
          *********************************************************************/
            } else if (strcmp(cmd, "LIST") == 0) {
             // LIST <subdir> -> list files in ~/S4/<subdir>
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
                 snprintf(dirPath, sizeof(dirPath), "%s/S4", home);
             } else {
                 if (strncmp(path, "~S4", 3) == 0) {
                     path += 3;
                     if (*path == '/') {
                         path++;
                     }
                 }
                 snprintf(dirPath, sizeof(dirPath), "%s/S4/%s", home, path);
             }
 
             DIR *dp = opendir(dirPath);
             if (!dp) {
                 // if directory doesn't exist, send "0\n"
                 const char *err = "0\n";
                 send(clientSock, err, strlen(err), 0);
             } else {
                 char listBuf[2048];
                 listBuf[0] = '\0';
                 struct dirent *entry;
                 while ((entry = readdir(dp)) != NULL) {
                     if (entry->d_name[0] == '.') {
                         continue; // skip hidden files
                     }
                     if (entry->d_type == DT_REG) {
                         // Add to list
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
  * main: Sets up a listening socket on port 9004. For each connection from S1,
  * spawns a new thread to handle commands. Typically, S1 uses S4 for .zip
  * operations (uploadf .zip, downlf .zip, removef .zip, etc.).
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
 
     // Bind to local port 9004
     struct sockaddr_in servaddr;
     memset(&servaddr, 0, sizeof(servaddr));
     servaddr.sin_family      = AF_INET;
     servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
     servaddr.sin_port        = htons(S4_PORT);
 
     if (bind(servSock, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
         perror("bind");
         close(servSock);
         exit(EXIT_FAILURE);
     }
 
     if (listen(servSock, BACKLOG) < 0) {
         perror("listen");
         close(servSock);
         exit(EXIT_FAILURE);
     }
     LOG("Server listening on port %d", S4_PORT);
 
     // Accept loop
     while (1) {
         struct sockaddr_in cliaddr;
         socklen_t clilen = sizeof(cliaddr);
         int *clientSock = malloc(sizeof(int));
         if (!clientSock) {
             continue;
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
             pthread_detach(tid);
         }
     }
 
     close(servSock);
     return 0;
 }
 