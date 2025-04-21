/*****************************************************************************
 * S1.c
 *
 * Main server that listens for connections from w25clients and forks a child
 * for each client. Handles commands to upload, download, remove, and list files.
 * Also coordinates with S2, S3, S4 to store .pdf, .txt, and .zip files,
 * keeping .c files locally in ~/S1.
 *
 * Build example (on Linux/Unix):
 *     gcc S1.c -o S1
 * Usage:
 *     ./S1
 *
 * Assumptions / Requirements:
 *  - The directories ~/S1, ~/S2, ~/S3, and ~/S4 already exist (not auto-created).
 *  - All servers (S1, S2, S3, S4) run on localhost with hardcoded ports.
 *  - S1 listens for w25clients on port 9001.
 *  - S2, S3, and S4 run on ports 9002, 9003, 9004 respectively.
 *
 *****************************************************************************/

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <errno.h>// Defines macros for reporting and handling errors
 #include <fcntl.h>
 #include <sys/types.h>
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
 #include <pthread.h>
 #include <sys/stat.h>
 #include <signal.h>
 #include <dirent.h>
 
 // ----------------------- CONFIGURATION CONSTANTS ----------------------------
 
 // Ports for the various servers
 #define S1_PORT 50004   // S1 listens on this port for client connections
 #define S2_PORT 50005   // S2 server port (for .pdf files)
 #define S3_PORT 50006   // S3 server port (for .txt files)
 #define S4_PORT 50007   // S4 server port (for .zip files)
 
 // Addresses (assuming everything is on localhost in this demo)
 #define S1_ADDR "127.0.0.1"
 #define S2_ADDR "127.0.0.1"
 #define S3_ADDR "127.0.0.1"
 #define S4_ADDR "127.0.0.1"
 
 // Buffer sizes
 #define MAX_CMD_LEN 1024   // Maximum length of a command string from client
 #define BUF_SIZE 4096      // Buffer size for file transfers
 
 // ----------------------- LOGGING MACRO & UTILITY ----------------------------
 
 // Simple logging macro that prints to stderr
 #define LOG(msg, ...) fprintf(stderr, "S1: " msg "\n", ##__VA_ARGS__)
 
 // ----------------------- FUNCTION DECLARATIONS ------------------------------
 
 // Main function that handles each connected client in a child process
 void prcclient(int clientSock);
 
 // Utility to ensure the specified directory path exists (creates subdirs if needed).
 // Return 0 on success, -1 on error.
 int ensure_directory_exists(const char *path);
 
 // Safe "send all" function to handle partial sends.
 int send_all(int sock, const void *buffer, size_t length);
 
 // Safe "receive all" function to read exactly `length` bytes.
 int recv_all(int sock, void *buffer, size_t length);
 
 // ---- Command-specific handlers ----
 int handle_upload(int clientSock, const char *filename, const char *destPath, long fileSize);
 int handle_download(int clientSock, const char *filePath);
 int handle_remove(int clientSock, const char *filePath);
 int handle_downltar(int clientSock, const char *fileType);
 int handle_dispfnames(int clientSock, const char *dirPath);
 
 // ----------------------- MAIN FUNCTION (S1 SERVER) --------------------------
 
 int main() {
     // Attempt to get HOME environment variable (for building ~/S1, etc.)
     char *homeDir = getenv("HOME");
     if (!homeDir) {
         fprintf(stderr, "Error: HOME environment variable not set.\n");
         exit(EXIT_FAILURE);
     }
 
     // Use SIGCHLD handler that automatically reaps child processes.
     // Alternatively, you could do signal(SIGCHLD, SIG_IGN);
     signal(SIGCHLD, SIG_IGN);
 
     // Create a listening socket
     int listenSock = socket(AF_INET, SOCK_STREAM, 0);
     if (listenSock < 0) {
         perror("socket");
         exit(EXIT_FAILURE);
     }
 
     // Allow reuse of port immediately after program ends
     int optval = 1;
     setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
 
     // Bind to an address/port
     struct sockaddr_in servaddr;
     memset(&servaddr, 0, sizeof(servaddr));
     servaddr.sin_family = AF_INET;
     servaddr.sin_addr.s_addr = htonl(INADDR_ANY);  // Listen on all interfaces
     servaddr.sin_port = htons(S1_PORT);
 
     if (bind(listenSock, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
         perror("bind");
         close(listenSock);
         exit(EXIT_FAILURE);
     }
 
     // Start listening
     if (listen(listenSock, 10) < 0) {
         perror("listen");
         close(listenSock);
         exit(EXIT_FAILURE);
     }
 
     LOG("Server is listening on port %d", S1_PORT);
 
     // Accept loop: fork a child for each new client
     while (1) {
         struct sockaddr_in clientAddr;
         socklen_t clientLen = sizeof(clientAddr);
         int clientSock = accept(listenSock, (struct sockaddr*)&clientAddr, &clientLen);
         if (clientSock < 0) {
             if (errno == EINTR) {
                 // Interrupted by signal, just retry
                 continue;
             }
             perror("accept");
             continue;
         }
 
         // Log that a new client is connecting 
         char clientIP[INET_ADDRSTRLEN];
         inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
         LOG("Accepted connection from %s:%d", clientIP, ntohs(clientAddr.sin_port));
 
         // Fork a child process to handle this client
         pid_t pid = fork();
         if (pid < 0) {
             perror("fork");
             close(clientSock);
             continue;
         }
         if (pid == 0) {
             // Child process
             close(listenSock);  // Child doesn't need the main listening socket
             prcclient(clientSock);
             close(clientSock);
             LOG("Client handled. Child exiting...");
             exit(0);
         } else {
             // Parent process
             close(clientSock);  // parent no longer needs this socket
         }
     }
 
     // Cleanup (though we never actually reach here in typical usage)
     close(listenSock);
     return 0;
 }
 
 // ----------------------- UTILITY FUNCTION DEFINITIONS -----------------------
 
 /**
  * @brief Creates the directory path recursively if it doesn't exist.
  * @param path Full path to ensure
  * @return 0 on success, -1 on error
  */
 int ensure_directory_exists(const char *path) {
     struct stat st;
     // If path already exists
     if (stat(path, &st) == 0) {
         if (S_ISDIR(st.st_mode)) {
             return 0; // Already a directory
         } else {
             // Exists but not a directory
             return -1;
         }
     }
     // We'll build it piece by piece
     char tmp[512];
     snprintf(tmp, sizeof(tmp), "%s", path);
     size_t len = strlen(tmp);
     if (len == 0) return -1;
     if (tmp[len - 1] == '/') {
         tmp[len - 1] = '\0';
     }
     // Create parent directories first
     for (char *p = tmp + 1; *p; p++) {
         if (*p == '/') {
             *p = '\0';
             if (stat(tmp, &st) != 0) {
                 if (mkdir(tmp, 0755) < 0) {
                     perror("mkdir");
                     return -1;
                 }
             } else if (!S_ISDIR(st.st_mode)) {
                 return -1;
             }
             *p = '/';
         }
     }
     // Create final directory
     if (stat(tmp, &st) != 0) {
         if (mkdir(tmp, 0755) < 0) {
             perror("mkdir");
             return -1;
         }
     } else if (!S_ISDIR(st.st_mode)) {
         return -1;
     }
     return 0;
 }
 
 /**
  * @brief Sends all data in a buffer to a socket, handling partial writes.
  * @param sock The socket file descriptor
  * @param buffer Data to send
  * @param length Number of bytes to send
  * @return 0 on success, -1 on error
  */
 int send_all(int sock, const void *buffer, size_t length) {
     size_t totalSent = 0;
     const char *buf = (const char*) buffer;
     while (totalSent < length) {
         ssize_t sent = send(sock, buf + totalSent, length - totalSent, 0);
         if (sent < 0) {
             return -1; // error
         }
         totalSent += sent;
     }
     return 0;
 }
 
 /**
  * @brief Reads exactly `length` bytes from a socket, handling partial reads.
  * @param sock The socket file descriptor
  * @param buffer Buffer to store data
  * @param length Number of bytes to read
  * @return 0 on success, -1 on error or if connection is closed prematurely
  */
 int recv_all(int sock, void *buffer, size_t length) {
     size_t totalRecv = 0;
     char *buf = (char*) buffer;
     while (totalRecv < length) {
         ssize_t n = recv(sock, buf + totalRecv, length - totalRecv, 0);
         if (n <= 0) {
             return -1; // error or connection closed
         }
         totalRecv += n;
     }
     return 0;
 }
 
 // ----------------------- CHILD PROCESS CLIENT HANDLER -----------------------
 
 /**
  * @brief Handles all commands from a single client. Runs in the child process.
  * @param clientSock The connected socket to the client
  */
 void prcclient(int clientSock) {
     char cmdBuf[MAX_CMD_LEN];
 
     while (1) {
         // Read a command line from the client. We'll parse it character by character until newline.
         int idx = 0;
         char c;
         while (1) {
             ssize_t n = recv(clientSock, &c, 1, 0);
             if (n <= 0) {
                 // Connection closed or error
                 return;
             }
             if (c == '\n') {
                 // End of command
                 break;
             }
             // Build up the command string
             if (idx < (MAX_CMD_LEN - 1)) {
                 cmdBuf[idx++] = c;
             }
         }
         cmdBuf[idx] = '\0';
 
         // If the command line is empty, ignore it
         if (idx == 0) {
             continue;
         }
 
         LOG("Received command: %s", cmdBuf);
 
         // Tokenize the command
         char *command = strtok(cmdBuf, " ");
         if (!command) {
             continue;
         }
 
         // Handle each possible command
         if (strcmp(command, "uploadf") == 0) {
             // Format: uploadf <filename> <dest_path> <filesize>
             char *filename = strtok(NULL, " ");
             char *destPath = strtok(NULL, " ");
             char *sizeStr  = strtok(NULL, " ");
             if (!filename || !destPath || !sizeStr) {
                 const char *errMsg = "ERROR: Invalid uploadf command format\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 continue;
             }
             long fileSize = atol(sizeStr);
             if (fileSize < 0) {
                 const char *errMsg = "ERROR: Invalid file size\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 continue;
             }
             // Handle the upload
             int res = handle_upload(clientSock, filename, destPath, fileSize);
             if (res == 0) {
                 const char *msg = "SUCCESS: File uploaded\n";
                 send_all(clientSock, msg, strlen(msg));
             } else {
                 const char *msg = "ERROR: File upload failed\n";
                 send_all(clientSock, msg, strlen(msg));
             }
 
         } else if (strcmp(command, "downlf") == 0) {
             // Format: downlf <file_path>
             char *filePath = strtok(NULL, "");
             if (!filePath) {
                 const char *errMsg = "ERROR: Invalid downlf command format\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 continue;
             }
             // Trim leading spaces from filePath
             while (*filePath == ' ') filePath++;
             if (strlen(filePath) == 0) {
                 const char *errMsg = "ERROR: Invalid file path\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 continue;
             }
             // Let the handler send the file or error
             handle_download(clientSock, filePath);
 
         } else if (strcmp(command, "removef") == 0) {
             // Format: removef <file_path>
             char *filePath = strtok(NULL, "");
             if (!filePath) {
                 const char *errMsg = "ERROR: Invalid removef command format\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 continue;
             }
             // Trim leading spaces
             while (*filePath == ' ') filePath++;
             if (strlen(filePath) == 0) {
                 const char *errMsg = "ERROR: Invalid file path\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 continue;
             }
             int res = handle_remove(clientSock, filePath);
             if (res == 0) {
                 const char *msg = "SUCCESS: File removed\n";
                 send_all(clientSock, msg, strlen(msg));
             } else {
                 const char *msg = "ERROR: File not found or cannot remove\n";
                 send_all(clientSock, msg, strlen(msg));
             }
 
         } else if (strcmp(command, "downltar") == 0) {
             // Format: downltar <filetype>
             char *fileType = strtok(NULL, " ");
             if (!fileType) {
                 const char *errMsg = "ERROR: Invalid downltar command format\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 continue;
             }
             handle_downltar(clientSock, fileType);
 
         } else if (strcmp(command, "dispfnames") == 0) {
             // Format: dispfnames <directory_path>
             char *dirPath = strtok(NULL, "");
             if (!dirPath) {
                 const char *errMsg = "ERROR: Invalid dispfnames command format\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 continue;
             }
             while (*dirPath == ' ') dirPath++;
             if (strlen(dirPath) == 0) {
                 const char *errMsg = "ERROR: Invalid directory path\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 continue;
             }
             handle_dispfnames(clientSock, dirPath);
 
         } else {
             // Unknown command
             const char *errMsg = "ERROR: Unknown command\n";
             send_all(clientSock, errMsg, strlen(errMsg));
         }
     }
 }
 
 // ----------------------- COMMAND HANDLER DEFINITIONS ------------------------
 
 /**
  * @brief Handles 'uploadf' command: receives file bytes from client and
  *        stores them in ~/S1 if .c, else forwards them to S2 (pdf), S3 (txt), or S4 (zip).
  */
 int handle_upload(int clientSock, const char *filename, const char *destPath, long fileSize) {
     // Identify file extension
     const char *ext = strrchr(filename, '.');
     if (!ext) {
         // No extension found
         LOG("Upload error: file has no extension");
         // Drain incoming data from socket to keep it in sync
         char buffer[BUF_SIZE];
         long remaining = fileSize;
         while (remaining > 0) {
             ssize_t r = recv(clientSock, buffer, (remaining < BUF_SIZE ? remaining : BUF_SIZE), 0);
             if (r <= 0) break;
             remaining -= r;
         }
         return -1;
     }
 
     // Build S1 base path: ~/S1
     char *homeDir = getenv("HOME");
     if (!homeDir) return -1;
     char basePath[512];
     snprintf(basePath, sizeof(basePath), "%s/S1", homeDir);
 
     // Replace "~S1" with basePath
     char destCopy[512];
     strncpy(destCopy, destPath, sizeof(destCopy) - 1);
     destCopy[sizeof(destCopy) - 1] = '\0';
     // Remove trailing slash if present
     size_t destLen = strlen(destCopy);
     if (destLen > 0 && destCopy[destLen - 1] == '/') {
         destCopy[destLen - 1] = '\0';
     }
     const char *subPath = destCopy;
     if (strncmp(destCopy, "~S1", 3) == 0) {
         subPath = destCopy + 3;
         if (*subPath == '/') subPath++;
     }
 
     // Build full directory path inside ~/S1
     char fullDir[512];
     if (*subPath) {
         snprintf(fullDir, sizeof(fullDir), "%s/%s", basePath, subPath);
     } else {
         snprintf(fullDir, sizeof(fullDir), "%s", basePath);
     }
 
     // Make sure the directory exists (but the instructions say user may have already created them).
     if (ensure_directory_exists(fullDir) != 0) {
         LOG("Directory creation failed for %s", fullDir);
         // Drain data from socket
         char buffer[BUF_SIZE];
         long remaining = fileSize;
         while (remaining > 0) {
             ssize_t r = recv(clientSock, buffer, (remaining < BUF_SIZE ? remaining : BUF_SIZE), 0);
             if (r <= 0) break;
             remaining -= r;
         }
         return -1;
     }
 
     // Open local file
     char fullPath[1024];
     snprintf(fullPath, sizeof(fullPath), "%s/%s", fullDir, filename);
     FILE *fp = fopen(fullPath, "wb");
     if (!fp) {
         LOG("Failed to open %s for writing: %s", fullPath, strerror(errno));
         // Drain incoming data
         char buffer[BUF_SIZE];
         long remaining = fileSize;
         while (remaining > 0) {
             ssize_t r = recv(clientSock, buffer, (remaining < BUF_SIZE ? remaining : BUF_SIZE), 0);
             if (r <= 0) break;
             remaining -= r;
         }
         return -1;
     }
 
     // Receive file data from client
     long remaining = fileSize;
     char buf[BUF_SIZE];
     while (remaining > 0) {
         ssize_t r = recv(clientSock, buf, (remaining < BUF_SIZE ? remaining : BUF_SIZE), 0);
         if (r <= 0) {
             fclose(fp);
             LOG("Connection lost while receiving file");
             return -1;
         }
         fwrite(buf, 1, r, fp);
         remaining -= r;
     }
     fclose(fp);
 
     LOG("Received file %s (size %ld bytes)", fullPath, fileSize);
 
     // If file is .c, keep it in S1
     if (strcmp(ext, ".c") == 0) {
         return 0;
     }
 
     // Otherwise, forward to S2 (.pdf), S3 (.txt), or S4 (.zip). Then remove local copy.
     const char *serverAddr;
     int serverPort;
     if (strcmp(ext, ".pdf") == 0) {
         serverAddr = S2_ADDR; serverPort = S2_PORT;
     } else if (strcmp(ext, ".txt") == 0) {
         serverAddr = S3_ADDR; serverPort = S3_PORT;
     } else if (strcmp(ext, ".zip") == 0) {
         serverAddr = S4_ADDR; serverPort = S4_PORT;
     } else {
         LOG("Unsupported file extension: %s", ext);
         remove(fullPath);
         return -1;
     }
 
     // Connect to the target storage server
     int sfd = socket(AF_INET, SOCK_STREAM, 0);
     if (sfd < 0) {
         LOG("Socket creation failed for forwarding");
         remove(fullPath);
         return -1;
     }
     struct sockaddr_in serv;
     memset(&serv, 0, sizeof(serv));
     serv.sin_family = AF_INET;
     serv.sin_port = htons(serverPort);
     if (inet_pton(AF_INET, serverAddr, &serv.sin_addr) <= 0) {
         LOG("Invalid address for server %s", serverAddr);
         close(sfd);
         remove(fullPath);
         return -1;
     }
     if (connect(sfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
         LOG("Could not connect to server for file forwarding");
         close(sfd);
         remove(fullPath);
         return -1;
     }
 
     // Construct relative path for server (replace ~S1 with their base).
     // Already have subPath for everything after ~S1
     char remotePath[512];
     if (*subPath) {
         snprintf(remotePath, sizeof(remotePath), "%s/%s", subPath, filename);
     } else {
         snprintf(remotePath, sizeof(remotePath), "%s", filename);
     }
     // Send the store command
     char header[600];
     snprintf(header, sizeof(header), "STORE %s %ld\n", remotePath, fileSize);
     if (send_all(sfd, header, strlen(header)) != 0) {
         LOG("Error sending STORE command");
         close(sfd);
         remove(fullPath);
         return -1;
     }
 
     // Forward file content
     FILE *fp_in = fopen(fullPath, "rb");
     if (!fp_in) {
         LOG("Cannot reopen file for forwarding");
         close(sfd);
         remove(fullPath);
         return -1;
     }
     char forwardBuf[BUF_SIZE];
     size_t bytesRead;
     while ((bytesRead = fread(forwardBuf, 1, BUF_SIZE, fp_in)) > 0) {
         if (send_all(sfd, forwardBuf, bytesRead) != 0) {
             LOG("Error forwarding file data");
             fclose(fp_in);
             close(sfd);
             remove(fullPath);
             return -1;
         }
     }
     fclose(fp_in);
 
     // Wait for server's response
     char ack[100];
     int ackIdx = 0;
     char ch;
     while (ackIdx < (int)sizeof(ack) - 1) {
         ssize_t n = recv(sfd, &ch, 1, 0);
         if (n <= 0) break;
         if (ch == '\n') break;
         ack[ackIdx++] = ch;
     }
     ack[ackIdx] = '\0';
     close(sfd);
 
     if (strncmp(ack, "SUCCESS", 7) != 0) {
         LOG("Server storing file responded with error: %s", ack);
         remove(fullPath);
         return -1;
     }
 
     // Remove the local copy after successful forwarding
     if (remove(fullPath) != 0) {
         LOG("Warning: could not remove local file %s (errno: %d)", fullPath, errno);
     }

     // Recursively remove empty directories from fullDir upward until basePath
    char currentDir[512];
    strncpy(currentDir, fullDir, sizeof(currentDir));

    // Loop: attempt to remove currentDir and then climb one level up
    while (strcmp(currentDir, basePath) != 0) {
        if (rmdir(currentDir) == 0) {
            LOG("Removed empty directory: %s", currentDir);
            // Find the last slash to move to the parent directory.
            char *lastSlash = strrchr(currentDir, '/');
            if (lastSlash != NULL) {
                *lastSlash = '\0';  // Remove the last directory component.
            } else {
                break;
            }
        } else {
            // rmdir failed—either because the directory is not empty or another error occurred.
            LOG("Directory %s not empty or could not be removed (errno: %d)", currentDir, errno);
            break;
        }
    }
 
     LOG("Forwarded file to storage server and removed local copy: %s", filename);
     return 0;
 }
 
 /**
  * @brief Handles 'downlf' command: obtains a file from S1 (if .c) or from S2/S3/S4 and sends it to the client.
  */
 int handle_download(int clientSock, const char *filePath) {
     // Determine file extension
     const char *ext = strrchr(filePath, '.');
     if (!ext) {
         const char *errMsg = "ERROR: Invalid file path\n";
         send_all(clientSock, errMsg, strlen(errMsg));
         return -1;
     }
 
     // Build local path from ~S1
     char *homeDir = getenv("HOME");
     if (!homeDir) {
         const char *errMsg = "ERROR: Internal error\n";
         send_all(clientSock, errMsg, strlen(errMsg));
         return -1;
     }
     char basePath[512];
     snprintf(basePath, sizeof(basePath), "%s/S1", homeDir);
 
     char pathCopy[512];
     strncpy(pathCopy, filePath, sizeof(pathCopy) - 1);
     pathCopy[sizeof(pathCopy) - 1] = '\0';
     size_t len = strlen(pathCopy);
     if (len > 0 && pathCopy[len - 1] == '/') {
         pathCopy[len - 1] = '\0';
     }
     const char *subPath = pathCopy;
     if (strncmp(pathCopy, "~S1", 3) == 0) {
         subPath = pathCopy + 3;
         if (*subPath == '/') subPath++;
     }
 
     char localPath[1024];
     snprintf(localPath, sizeof(localPath), "%s/%s", basePath, subPath);
 
     // If .c, read from local S1
     if (strcmp(ext, ".c") == 0) {
         FILE *fp = fopen(localPath, "rb");
         if (!fp) {
             const char *errMsg = "ERROR: File not found\n";
             send_all(clientSock, errMsg, strlen(errMsg));
             return -1;
         }
         // Send file size
         fseek(fp, 0, SEEK_END);
         long fileSize = ftell(fp);
         fseek(fp, 0, SEEK_SET);
         char sizeStr[64];
         snprintf(sizeStr, sizeof(sizeStr), "%ld\n", fileSize);
         if (send_all(clientSock, sizeStr, strlen(sizeStr)) != 0) {
             fclose(fp);
             return -1;
         }
         // Send file content
         char buffer[BUF_SIZE];
         size_t bytesRead;
         while ((bytesRead = fread(buffer, 1, BUF_SIZE, fp)) > 0) {
             if (send_all(clientSock, buffer, bytesRead) != 0) {
                 fclose(fp);
                 return -1;
             }
         }
         fclose(fp);
         LOG("Sent local file %s to client (%ld bytes)", localPath, fileSize);
         return 0;
     }
 
     // Otherwise, the file is on S2(.pdf), S3(.txt), or S4(.zip)
     const char *serverAddr;
     int serverPort;
     if (strcmp(ext, ".pdf") == 0) {
         serverAddr = S2_ADDR; serverPort = S2_PORT;
     } else if (strcmp(ext, ".txt") == 0) {
         serverAddr = S3_ADDR; serverPort = S3_PORT;
     } else {
         const char *errMsg = "ERROR: Unsupported file type\n";
         send_all(clientSock, errMsg, strlen(errMsg));
         return -1;
     }
 
     // Connect to the appropriate server
     int sfd = socket(AF_INET, SOCK_STREAM, 0);
     if (sfd < 0) {
         const char *errMsg = "ERROR: Internal error\n";
         send_all(clientSock, errMsg, strlen(errMsg));
         return -1;
     }
     struct sockaddr_in serv;
     memset(&serv, 0, sizeof(serv));
     serv.sin_family = AF_INET;
     serv.sin_port = htons(serverPort);
     if (inet_pton(AF_INET, serverAddr, &serv.sin_addr) <= 0) {
         const char *errMsg = "ERROR: Internal error\n";
         send_all(clientSock, errMsg, strlen(errMsg));
         close(sfd);
         return -1;
     }
     if (connect(sfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
         const char *errMsg = "ERROR: File server unavailable\n";
         send_all(clientSock, errMsg, strlen(errMsg));
         close(sfd);
         return -1;
     }
 
     // Send "GET path" command
     char cmd[600];
     snprintf(cmd, sizeof(cmd), "GET %s\n", subPath);
     if (send_all(sfd, cmd, strlen(cmd)) != 0) {
         const char *errMsg = "ERROR: Internal error\n";
         send_all(clientSock, errMsg, strlen(errMsg));
         close(sfd);
         return -1;
     }
 
     // Expect a response with file size or ERROR
     char line[128];
     int idx = 0;
     char ch;
     while (idx < (int)sizeof(line) - 1) {
         ssize_t n = recv(sfd, &ch, 1, 0);
         if (n <= 0) break;
         if (ch == '\n') break;
         line[idx++] = ch;
     }
     line[idx] = '\0';
 
     if (idx == 0) {
         // No response or connection closed
         const char *errMsg = "ERROR: Failed to retrieve file\n";
         send_all(clientSock, errMsg, strlen(errMsg));
         close(sfd);
         return -1;
     }
     if (strncmp(line, "ERROR", 5) == 0) {
         // Remote server error
         strcat(line, "\n"); // Ensure newline
         send_all(clientSock, line, strlen(line));
         close(sfd);
         return -1;
     }
 
     long fileSize = atol(line);
     if (fileSize < 0) {
         const char *errMsg = "ERROR: Failed to retrieve file\n";
         send_all(clientSock, errMsg, strlen(errMsg));
         close(sfd);
         return -1;
     }
 
     // Send size to client
     char sizeStr[64];
     snprintf(sizeStr, sizeof(sizeStr), "%ld\n", fileSize);
     if (send_all(clientSock, sizeStr, strlen(sizeStr)) != 0) {
         close(sfd);
         return -1;
     }
 
     // Relay the file content from server to client
     long remaining = fileSize;
     char buffer[BUF_SIZE];
     while (remaining > 0) {
         ssize_t r = recv(sfd, buffer, (remaining < BUF_SIZE ? remaining : BUF_SIZE), 0);
         if (r <= 0) break;
         if (send_all(clientSock, buffer, r) != 0) break;
         remaining -= r;
     }
     close(sfd);
 
     if (remaining == 0) {
         LOG("Downloaded file from server and relayed to client: %s (%ld bytes)", filePath, fileSize);
         return 0;
     } else {
         LOG("Error relaying file %s", filePath);
         return -1;
     }
 }
 
 /**
  * @brief Handles 'removef' command: deletes a file locally if .c, otherwise instructs S2/S3/S4 to delete it.
  */
 int handle_remove(int clientSock, const char *filePath) {
     const char *ext = strrchr(filePath, '.');
     if (!ext) return -1;
 
     char *homeDir = getenv("HOME");
     if (!homeDir) return -1;
     char basePath[512];
     snprintf(basePath, sizeof(basePath), "%s/S1", homeDir);
 
     char pathCopy[512];
     strncpy(pathCopy, filePath, sizeof(pathCopy) - 1);
     pathCopy[sizeof(pathCopy) - 1] = '\0';
     size_t len = strlen(pathCopy);
     if (len > 0 && pathCopy[len - 1] == '/') {
         pathCopy[len - 1] = '\0';
     }
     const char *subPath = pathCopy;
     if (strncmp(pathCopy, "~S1", 3) == 0) {
         subPath = pathCopy + 3;
         if (*subPath == '/') subPath++;
     }
     char fullPath[1024];
     snprintf(fullPath, sizeof(fullPath), "%s/%s", basePath, subPath);
 
     /*// If .c, remove locally
     if (strcmp(ext, ".c") == 0) {
         if (unlink(fullPath) == 0) {
             LOG("Removed local .c file: %s", fullPath);
             return 0;
         } else {
             LOG("Failed to remove local file %s: %s", fullPath, strerror(errno));
             return -1;
         }
     }*/

        // If .c, remove locally and then clean up empty parent directories.
    if (strcmp(ext, ".c") == 0) {
        if (unlink(fullPath) == 0) {
            LOG("Removed local .c file: %s", fullPath);
            
            // Start cleaning up empty directories.
            // Copy fullPath to a temporary buffer (we already removed the file, so we need its parent directory).
            char currentDir[1024];
            strncpy(currentDir, fullPath, sizeof(currentDir));
            currentDir[sizeof(currentDir) - 1] = '\0';

            // Remove the file name from currentDir to get the directory containing the file.
            char *lastSlash = strrchr(currentDir, '/');
            if (lastSlash != NULL) {
                *lastSlash = '\0';
            } else {
                // Cannot determine the directory; just return.
                return 0;
            }
            
            // Remove directories upward until we reach the basePath.
            while (strcmp(currentDir, basePath) != 0) {
                // Try to remove the directory.
                if (rmdir(currentDir) == 0) {
                    LOG("Removed empty directory: %s", currentDir);
                    // Remove the last path component to climb one level.
                    lastSlash = strrchr(currentDir, '/');
                    if (lastSlash != NULL) {
                        *lastSlash = '\0';
                    } else {
                        // Reached a point where we can't get a parent, so break.
                        break;
                    }
                } else {
                    // If the directory isn't empty (or an error occurs), stop the loop.
                    LOG("Directory %s not empty or could not be removed (errno: %d), stopping cleanup", currentDir, errno);
                    break;
                }
            }
            return 0;
        } else {
            LOG("Failed to remove local file %s: %s", fullPath, strerror(errno));
            return -1;
        }
    }
    
     // Otherwise, forward the request to the appropriate server
     const char *serverAddr;
     int serverPort;
     if (strcmp(ext, ".pdf") == 0) {
         serverAddr = S2_ADDR; serverPort = S2_PORT;
     } else if (strcmp(ext, ".txt") == 0) {
         serverAddr = S3_ADDR; serverPort = S3_PORT;
     } else {
         return -1;
     }
 
     int sfd = socket(AF_INET, SOCK_STREAM, 0);
     if (sfd < 0) {
         return -1;
     }
     struct sockaddr_in serv;
     memset(&serv, 0, sizeof(serv));
     serv.sin_family = AF_INET;
     serv.sin_port = htons(serverPort);
     if (inet_pton(AF_INET, serverAddr, &serv.sin_addr) <= 0) {
         close(sfd);
         return -1;
     }
     if (connect(sfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
         close(sfd);
         return -1;
     }
 
     char cmd[600];
     snprintf(cmd, sizeof(cmd), "DEL %s\n", subPath);
     if (send_all(sfd, cmd, strlen(cmd)) != 0) {
         close(sfd);
         return -1;
     }
 
     // Read server ack
     char ack[64];
     int idx = 0;
     char ch;
     while (idx < (int)sizeof(ack) - 1) {
         ssize_t n = recv(sfd, &ch, 1, 0);
         if (n <= 0) break;
         if (ch == '\n') break;
         ack[idx++] = ch;
     }
     ack[idx] = '\0';
     close(sfd);
 
    if (strncmp(ack, "SUCCESS", 7) == 0) {
         LOG("Remote server removed file: %s", filePath);
         return 0;
     } else {
         LOG("Remote server failed to remove file: %s (%s)", filePath, ack);
         return -1;
     }
 }
 
// Handles "downltar" command.
int handle_downltar(int clientSock, const char *fileType) {
    // Validate file type.
    if (strcmp(fileType, ".c") != 0 && strcmp(fileType, ".pdf") != 0 &&
        strcmp(fileType, ".txt") != 0 && strcmp(fileType, ".zip") != 0) {
         const char *errMsg = "ERROR: Invalid filetype (supported: .c, .pdf, .txt, .zip)\n";
         send_all(clientSock, errMsg, strlen(errMsg));
         return -1;
    }
    // Case for .c files: archive files stored in S1 directory
    if (strcmp(fileType, ".c") == 0) {
         // Get S1 directory path from environment variable, with fallback to current directory
         const char *s1Path = getenv("S1_DIRECTORY");
         if (s1Path == NULL) {
             // If environment variable is not set, try the home directory approach
             const char *homeDir = getenv("HOME");
             if (homeDir == NULL) {
                 const char *errMsg = "ERROR: Could not determine S1 directory location\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 return -1;
             }
             // Allocate space for home directory path + "/S1"
             char *s1PathBuf = malloc(strlen(homeDir) + 4);
             if (s1PathBuf == NULL) {
                 const char *errMsg = "ERROR: Memory allocation failed\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 return -1;
             }
             sprintf(s1PathBuf, "%s/S1", homeDir);
             s1Path = s1PathBuf;
         }
         
         // Verify S1 directory exists
         struct stat st;
         if (stat(s1Path, &st) != 0 || !S_ISDIR(st.st_mode)) {
              const char *errMsg = "ERROR: S1 directory not found\n";
              send_all(clientSock, errMsg, strlen(errMsg));
              if (s1Path != getenv("S1_DIRECTORY")) {
                  free((void*)s1Path);
              }
              return -1;
         }
         
         // Create a unique temporary filename for the tar archive.
         char template[] = "/tmp/cfilesXXXXXX";
         int temp_fd = mkstemp(template);
         if (temp_fd == -1) {
              const char *errMsg = "ERROR: Unable to create temporary file\n";
              send_all(clientSock, errMsg, strlen(errMsg));
              if (s1Path != getenv("S1_DIRECTORY")) {
                  free((void*)s1Path);
              }
              return -1;
         }
         close(temp_fd); // Close immediately; tar will overwrite it.
         
         // Create the tar file - using the configurable path
         char tarCmd[1024];
         snprintf(tarCmd, sizeof(tarCmd), 
                 "cd %s && find . -name \"*.c\" -print0 | tar --null -T - -cf %s 2>/dev/null || echo 'empty'",
                 s1Path, template);
         
         LOG("Executing tar command: %s", tarCmd);
         int status = system(tarCmd);
         
         // Free the allocated path if necessary
         if (s1Path != getenv("S1_DIRECTORY")) {
             free((void*)s1Path);
         }
         
         // If tar failed or returned nothing, create an empty tar file
         struct stat tar_st;
         if (stat(template, &tar_st) != 0 || tar_st.st_size == 0) {
             LOG("Creating empty tar file");
             FILE *emptyTar = fopen(template, "wb");
             if (!emptyTar) {
                 const char *errMsg = "ERROR: Failed to create empty tar file\n";
                 send_all(clientSock, errMsg, strlen(errMsg));
                 remove(template);
                 return -1;
             }
             fclose(emptyTar);
         }
         
         // Open the tar archive.
         FILE *fp = fopen(template, "rb");
         if (!fp) {
              const char *errMsg = "ERROR: Tar file not found\n";
              send_all(clientSock, errMsg, strlen(errMsg));
              remove(template);
              return -1;
         }
         fseek(fp, 0, SEEK_END);
         long tarSize = ftell(fp);
         fseek(fp, 0, SEEK_SET);
         
         // Send tar archive size as a newline-terminated string.
         char sizeStr[64];
         snprintf(sizeStr, sizeof(sizeStr), "%ld\n", tarSize);
         if (send_all(clientSock, sizeStr, strlen(sizeStr)) != 0) {
              fclose(fp);
              remove(template);
              return -1;
         }
         
         // Send tar archive data.
         char buffer[BUF_SIZE];
         size_t bytes;
         while ((bytes = fread(buffer, 1, BUF_SIZE, fp)) > 0) {
              if (send_all(clientSock, buffer, bytes) != 0) {
                   fclose(fp);
                   remove(template);
                   return -1;
              }
         }
         fclose(fp);
         remove(template);
         LOG("Sent tar archive for .c files to client (%ld bytes)", tarSize);
         return 0;
    }
    // For .pdf, .txt, and forward the request to the appropriate remote server.
    else if (strcmp(fileType, ".pdf") == 0 || strcmp(fileType, ".txt") == 0) {
         const char *serverAddr;
         int serverPort;
         if (strcmp(fileType, ".pdf") == 0) {
              serverAddr = S2_ADDR; serverPort = S2_PORT;
         } else if (strcmp(fileType, ".txt") == 0) {
              serverAddr = S3_ADDR; serverPort = S3_PORT;
         } else {
              const char *errMsg = "ERROR: Internal error (invalid file type)\n";
              send_all(clientSock, errMsg, strlen(errMsg));
              return -1;
         }
         int sfd = socket(AF_INET, SOCK_STREAM, 0);
         if (sfd < 0) {
              const char *errMsg = "ERROR: Internal error (socket creation failed)\n";
              send_all(clientSock, errMsg, strlen(errMsg));
              return -1;
         }
         struct sockaddr_in serv;
         memset(&serv, 0, sizeof(serv));
         serv.sin_family = AF_INET;
         serv.sin_port = htons(serverPort);
         if (inet_pton(AF_INET, serverAddr, &serv.sin_addr) <= 0) {
              const char *errMsg = "ERROR: Internal error (address conversion failed)\n";
              send_all(clientSock, errMsg, strlen(errMsg));
              close(sfd);
              return -1;
         }
         if (connect(sfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
              const char *errMsg = "ERROR: File server unavailable\n";
              send_all(clientSock, errMsg, strlen(errMsg));
              close(sfd);
              return -1;
         }
         // Send "TAR" command with file type.
         char tarCmd[32];
         snprintf(tarCmd, sizeof(tarCmd), "TAR%s\n", fileType);
         if (send_all(sfd, tarCmd, strlen(tarCmd)) != 0) {
              const char *errMsg = "ERROR: Internal error (failed to send command)\n";
              send_all(clientSock, errMsg, strlen(errMsg));
              close(sfd);
              return -1;
         }
         // Expect tar archive size as a newline-terminated string.
         char line[128];
         int idx = 0;
         char ch;
         while (idx < (int)sizeof(line) - 1) {
              ssize_t n = recv(sfd, &ch, 1, 0);
              if (n <= 0) break;
              if (ch == '\n') break;
              line[idx++] = ch;
         }
         line[idx] = '\0';
         if (idx == 0 || strncmp(line, "ERROR", 5) == 0) {
              if (idx == 0) {
                   const char *errMsg = "ERROR: Tar failed (no response from server)\n";
                   send_all(clientSock, errMsg, strlen(errMsg));
              } else {
                   strcat(line, "\n");
                   send_all(clientSock, line, strlen(line));
              }
              close(sfd);
              return -1;
         }
         long tarSize = atol(line);
         char sizeStrOut[64];
         snprintf(sizeStrOut, sizeof(sizeStrOut), "%ld\n", tarSize);
         if (send_all(clientSock, sizeStrOut, strlen(sizeStrOut)) != 0) {
              close(sfd);
              return -1;
         }
         long remaining = tarSize;
         char buffer[BUF_SIZE];
         while (remaining > 0) {
              ssize_t r = recv(sfd, buffer, (remaining < BUF_SIZE ? remaining : BUF_SIZE), 0);
              if (r <= 0) break;
              if (send_all(clientSock, buffer, r) != 0) break;
              remaining -= r;
         }
         close(sfd);
         if (remaining == 0) {
              LOG("Relayed tar of type %s (%ld bytes) to client", fileType, tarSize);
              return 0;
         } else {
              LOG("Error relaying tar file of type %s (%ld bytes remaining)", fileType, remaining);
              return -1;
         }
    }
    // Should not reach here.
    const char *errMsg = "ERROR: Unsupported filetype\n";
    send_all(clientSock, errMsg, strlen(errMsg));
    return -1;
}
 
 /**
  * @brief Handles 'dispfnames' command: aggregates filenames from local .c and remote servers for .pdf, .txt, .zip.
  */
 int handle_dispfnames(int clientSock, const char *dirPath) {
     // Convert ~S1 path to actual local path
     char *homeDir = getenv("HOME");
     if (!homeDir) {
         const char *errMsg = "ERROR: Internal error\n";
         send_all(clientSock, errMsg, strlen(errMsg));
         return -1;
     }
     char basePath[512];
     snprintf(basePath, sizeof(basePath), "%s/S1", homeDir);
 
     char pathCopy[512];
     strncpy(pathCopy, dirPath, sizeof(pathCopy) - 1);
     pathCopy[sizeof(pathCopy) - 1] = '\0';
     size_t len = strlen(pathCopy);
     if (len > 0 && pathCopy[len - 1] == '/') {
         pathCopy[len - 1] = '\0';
     }
     const char *subPath = pathCopy;
     if (strncmp(pathCopy, "~S1", 3) == 0) {
         subPath = pathCopy + 3;
         if (*subPath == '/') subPath++;
     }
 
     // Construct local directory
     char localDir[1024];
     if (*subPath) {
         snprintf(localDir, sizeof(localDir), "%s/%s", basePath, subPath);
     } else {
         snprintf(localDir, sizeof(localDir), "%s", basePath);
     }
 
     // We'll collect .c, .pdf, .txt, and .zip from the different locations
     // 1) local .c
     char *cFiles[256];   int cCount   = 0;
     char *pdfFiles[256]; int pdfCount = 0;
     char *txtFiles[256]; int txtCount = 0;
     char *zipFiles[256]; int zipCount = 0;
 
     // Initialize counts
     cCount = pdfCount = txtCount = zipCount = 0;
 
     // Read local .c files
     DIR *dp = opendir(localDir);
     if (dp) {
         struct dirent *entry;
         while ((entry = readdir(dp)) != NULL) {
             if (entry->d_type == DT_REG) {
                 const char *name = entry->d_name;
                 const char *ext = strrchr(name, '.');
                 if (ext && strcmp(ext, ".c") == 0) {
                     // Save a copy of the filename
                     cFiles[cCount++] = strdup(name);
                     if (cCount >= 256) break;
                 }
             }
         }
         closedir(dp);
     }
     // If dp == NULL, directory might not exist locally, so cCount stays 0.
 
     // We'll define a small helper lambda to connect to S2/S3/S4, issue a "LIST" command, and parse the result
     // (In C99, we’ll just define a function instead of a lambda).
     // For clarity, here it is inline:
     int connect_and_list(const char *addr, int port, char **files, int *count) {
         int sfd = socket(AF_INET, SOCK_STREAM, 0);
         if (sfd < 0) return -1;
         struct sockaddr_in serv;
         memset(&serv, 0, sizeof(serv));
         serv.sin_family = AF_INET;
         serv.sin_port = htons(port);
         if (inet_pton(AF_INET, addr, &serv.sin_addr) <= 0) {
             close(sfd);
             return -1;
         }
         if (connect(sfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
             close(sfd);
             return -1;
         }
         // Send "LIST subPath\n" (or .)
         char cmd[512];
         if (*subPath) {
             snprintf(cmd, sizeof(cmd), "LIST %s\n", subPath);
         } else {
             snprintf(cmd, sizeof(cmd), "LIST .\n");
         }
         if (send_all(sfd, cmd, strlen(cmd)) != 0) {
             close(sfd);
             return -1;
         }
         // Receive size or "0"
         char line[128];
         int idx = 0;
         char c;
         while (idx < (int)sizeof(line) - 1) {
             ssize_t n = recv(sfd, &c, 1, 0);
             if (n <= 0) {
                 close(sfd);
                 return -1;
             }
             if (c == '\n') break;
             line[idx++] = c;
         }
         line[idx] = '\0';
         long listSize = atol(line);
         if (listSize <= 0) {
             close(sfd);
             return 0; // no files or error
         }
         // Now read exactly listSize bytes
         char *buf = (char*)malloc(listSize + 1);
         if (!buf) {
             close(sfd);
             return -1;
         }
         long remaining = listSize;
         char *p = buf;
         while (remaining > 0) {
             ssize_t r = recv(sfd, p, remaining, 0);
             if (r <= 0) {
                 free(buf);
                 close(sfd);
                 return -1;
             }
             p += r;
             remaining -= r;
         }
         buf[listSize] = '\0';
         close(sfd);
 
         // Tokenize by newline to get filenames
         char *saveptr;
         char *token = strtok_r(buf, "\n", &saveptr);
         while (token) {
             if (*token != '\0') {
                 files[*count] = strdup(token);
                 (*count)++;
                 if (*count >= 256) break;
             }
             token = strtok_r(NULL, "\n", &saveptr);
         }
         free(buf);
         return 0;
     };
 
     // Retrieve .pdf from S2, .txt from S3, .zip from S4
     connect_and_list(S2_ADDR, S2_PORT, pdfFiles, &pdfCount);
     connect_and_list(S3_ADDR, S3_PORT, txtFiles, &txtCount);
     connect_and_list(S4_ADDR, S4_PORT, zipFiles, &zipCount);
 
     // Sort each group alphabetically
     int cmpfunc(const void *a, const void *b) {
         const char *fa = *(const char**)a;
         const char *fb = *(const char**)b;
         return strcmp(fa, fb);
     }
     if (cCount > 0)   qsort(cFiles,   cCount,   sizeof(char*), cmpfunc);
     if (pdfCount > 0) qsort(pdfFiles, pdfCount, sizeof(char*), cmpfunc);
     if (txtCount > 0) qsort(txtFiles, txtCount, sizeof(char*), cmpfunc);
     if (zipCount > 0) qsort(zipFiles, zipCount, sizeof(char*), cmpfunc);
 
     // Build consolidated output: .c first, then .pdf, then .txt, then .zip
     char output[2048];
     output[0] = '\0';
 
     // c
     for (int i = 0; i < cCount; i++) {
         strncat(output, cFiles[i], sizeof(output) - strlen(output) - 1);
         strncat(output, "\n", sizeof(output) - strlen(output) - 1);
         free(cFiles[i]);
     }
     // pdf
     for (int i = 0; i < pdfCount; i++) {
         strncat(output, pdfFiles[i], sizeof(output) - strlen(output) - 1);
         strncat(output, "\n", sizeof(output) - strlen(output) - 1);
         free(pdfFiles[i]);
     }
     // txt
     for (int i = 0; i < txtCount; i++) {
         strncat(output, txtFiles[i], sizeof(output) - strlen(output) - 1);
         strncat(output, "\n", sizeof(output) - strlen(output) - 1);
         free(txtFiles[i]);
     }
     // zip
     for (int i = 0; i < zipCount; i++) {
         strncat(output, zipFiles[i], sizeof(output) - strlen(output) - 1);
         strncat(output, "\n", sizeof(output) - strlen(output) - 1);
         free(zipFiles[i]);
     }
 
     if (strlen(output) == 0) {
         const char *msg = "No files found\n";
         send_all(clientSock, msg, strlen(msg));
     } else {
         // We’ll send length first, then the actual list
         long outLen = strlen(output);
         char lenStr[64];
         snprintf(lenStr, sizeof(lenStr), "%ld\n", outLen);
         send_all(clientSock, lenStr, strlen(lenStr));
         send_all(clientSock, output, outLen);
     }
     return 0;
 }