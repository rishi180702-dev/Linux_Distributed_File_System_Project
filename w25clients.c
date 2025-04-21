/*****************************************************************************
 * w25clients.c
 *
 * Client program that connects to the S1 server. The user can type:
 *
 *   1. uploadf <filename> <destination_path>
 *   2. downlf <file_path_in_S1>
 *   3. removef <file_path_in_S1>
 *   4. downltar <filetype>
 *   5. dispfnames <directory_path_in_S1>
 *
 * where <file_path_in_S1> or <directory_path_in_S1> typically starts with ~S1.
 *
 * The client reads each command, validates basic syntax, then sends it to S1.
 * For file transfers (upload/download/tar), it also handles sending/receiving
 * file contents.
 *
 * Build example (on Linux/Unix):
 *     gcc w25clients.c -o w25clients
 * Usage:
 *     ./w25clients
 *   or to specify a custom server IP/port:
 *     ./w25clients [S1_IP] [S1_port]
 *
 *****************************************************************************/

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <arpa/inet.h>
 #include <errno.h>
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <sys/stat.h>
 
 // Default connection settings for S1 (can be overridden via argv)
 #define DEFAULT_S1_PORT 50004
 #define DEFAULT_S1_ADDR "127.0.0.1"
 
 // Buffer size for file transfer
 #define BUF_SIZE 4096
 
 /*****************************************************************************
  * send_all: ensures we send the entire buffer over a socket, even if 'send()'
  * writes only part of it. Returns 0 on success, -1 on error.
  *****************************************************************************/
 int send_all(int sock, const void *buf, size_t len) {
     size_t sent = 0;
     const char *ptr = (const char*)buf;
     while (sent < len) {
         ssize_t n = send(sock, ptr + sent, len - sent, 0);
         if (n < 0) {
             return -1;  // error
         }
         sent += n;
     }
     return 0;
 }
 
 /*****************************************************************************
  * recv_line: reads data from the socket one character at a time until we hit
  * a newline or we've read maxlen-1 chars. Puts a '\0' terminator at the end.
  *
  * Returns:
  *   - number of bytes read if successful (including '\n'),
  *   - 0 if the server has closed the connection,
  *   - -1 on error.
  *****************************************************************************/
 int recv_line(int sock, char *buf, size_t maxlen) {
     size_t i = 0;
     char ch;
     while (i < maxlen - 1) {
         ssize_t n = recv(sock, &ch, 1, 0);
         if (n <= 0) {
             // 0 => connection closed, -1 => error
             return (n == 0) ? 0 : -1;
         }
         buf[i++] = ch;
         if (ch == '\n') {
             // we include the newline in our buffer, but we can break
             break;
         }
     }
     buf[i] = '\0';  // null-terminate
     return (int)i;
 }
 
 /*****************************************************************************
  * recv_all: reads exactly 'length' bytes from the socket, handling short reads.
  * Returns 0 on success, or -1 on error/disconnection.
  *****************************************************************************/
 int recv_all(int sock, void *buffer, size_t length) {
     size_t total = 0;
     char *buf = (char*)buffer;
     while (total < length) {
         ssize_t n = recv(sock, buf + total, length - total, 0);
         if (n <= 0) {
             return -1; // error or connection closed
         }
         total += n;
     }
     return 0;
 }
 
 /*****************************************************************************
  * main: Connects to S1 and continuously prompts the user for commands. Sends
  * commands to S1 and processes the responses (including file transmissions).
  *****************************************************************************/
 int main(int argc, char *argv[]) {
     // Parse optional command-line arguments (server IP, port)
     const char *serverIP = DEFAULT_S1_ADDR;
     int serverPort = DEFAULT_S1_PORT;
 
     if (argc >= 2) {
         serverIP = argv[1];       // custom IP
     }
     if (argc >= 3) {
         serverPort = atoi(argv[2]);  // custom port
     }
 
     // Create a socket
     int sock = socket(AF_INET, SOCK_STREAM, 0);
     if (sock < 0) {
         perror("socket");
         return EXIT_FAILURE;
     }
 
     // Prepare address structure
     struct sockaddr_in servaddr;
     memset(&servaddr, 0, sizeof(servaddr));
     servaddr.sin_family = AF_INET;
     servaddr.sin_port = htons(serverPort);
 
     if (inet_pton(AF_INET, serverIP, &servaddr.sin_addr) <= 0) {
         fprintf(stderr, "Invalid server address %s\n", serverIP);
         close(sock);
         return EXIT_FAILURE;
     }
 
     // Connect to S1
     if (connect(sock, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
         perror("connect");
         close(sock);
         return EXIT_FAILURE;
     }
 
     printf("Connected to S1 at %s:%d\n", serverIP, serverPort);
 
     // Main command loop
     char input[1024];
     while (1) {
         printf("w25clients$ ");
         fflush(stdout);
 
         // Read a line from stdin
         if (!fgets(input, sizeof(input), stdin)) {
             // EOF or error
             break;
         }
         // Remove trailing newline
         size_t len = strlen(input);
         if (len > 0 && input[len - 1] == '\n') {
             input[len - 1] = '\0';
         }
         // If empty input, keep going
         if (strlen(input) == 0) {
             continue;
         }
 
         // If user typed "quit" or "exit", break out
         if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
             break;
         }
 
         // Parse the command
         char *cmd = strtok(input, " ");
         if (!cmd) {
             continue;
         }
 
         // --------------- uploadf ---------------
         if (strcmp(cmd, "uploadf") == 0) {
             char *filename = strtok(NULL, " ");
             char *destPath = strtok(NULL, "");
             if (!filename || !destPath) {
                 fprintf(stderr, "Usage: uploadf <filename> <destination_path>\n");
                 continue;
             }
             // Trim leading spaces from destPath
             while (*destPath == ' ') {
                 destPath++;
             }
             // Check if the file exists locally
             struct stat st;
             if (stat(filename, &st) != 0) {
                 perror("File not found");
                 continue;
             }
             long fileSize = st.st_size;
 
             // Check extension
             const char *ext = strrchr(filename, '.');
             if (!ext || !(strcmp(ext, ".c") == 0 || strcmp(ext, ".pdf") == 0 ||
                           strcmp(ext, ".txt") == 0 || strcmp(ext, ".zip") == 0)) {
                 fprintf(stderr, "Error: uploadf supports only .c, .pdf, .txt, .zip\n");
                 continue;
             }
             // Check if destination starts with ~S1
             if (strncmp(destPath, "~S1", 3) != 0) {
                 fprintf(stderr, "Error: destination_path must begin with ~S1\n");
                 continue;
             }
 
             // Send the command line to S1, including file size
             char header[1024];
             snprintf(header, sizeof(header), "uploadf %s %s %ld\n", filename, destPath, fileSize);
             if (send_all(sock, header, strlen(header)) != 0) {
                 fprintf(stderr, "Failed to send 'uploadf' command\n");
                 continue;
             }
 
             // Now send the file data
             FILE *fp = fopen(filename, "rb");
             if (!fp) {
                 perror("fopen");
                 continue;
             }
             char buf[BUF_SIZE];
             size_t bytesRead;
             while ((bytesRead = fread(buf, 1, BUF_SIZE, fp)) > 0) {
                 if (send_all(sock, buf, bytesRead) != 0) {
                     fprintf(stderr, "Error sending file data\n");
                     break;
                 }
             }
             fclose(fp);
 
             // Receive server response
             char resp[256];
             int r = recv_line(sock, resp, sizeof(resp));
             if (r <= 0) {
                 fprintf(stderr, "Connection closed by server\n");
                 break;
             }
             // Print the response from S1
             printf("%s", resp);
 
         // --------------- downlf ---------------
         } else if (strcmp(cmd, "downlf") == 0) {
             char *path = strtok(NULL, "");
             if (!path) {
                 fprintf(stderr, "Usage: downlf <file_path_in_S1>\n");
                 continue;
             }
             // Trim spaces
             while (*path == ' ') {
                 path++;
             }
             if (strlen(path) == 0) {
                 fprintf(stderr, "Usage: downlf <file_path_in_S1>\n");
                 continue;
             }
             const char *ext = strrchr(path, '.');
             if (!ext || !(strcmp(ext, ".c") == 0 || strcmp(ext, ".pdf") == 0 ||
                           strcmp(ext, ".txt") == 0 || strcmp(ext, ".zip") == 0)) {
                 fprintf(stderr, "Error: unsupported file type for downlf\n");
                 continue;
             }
             // Must begin with ~S1
             if (strncmp(path, "~S1", 3) != 0) {
                 fprintf(stderr, "Error: file path must begin with ~S1\n");
                 continue;
             }
             // Send the downlf command
             char msg[1024];
             snprintf(msg, sizeof(msg), "downlf %s\n", path);
             if (send_all(sock, msg, strlen(msg)) != 0) {
                 fprintf(stderr, "Failed to send 'downlf' command\n");
                 continue;
             }
             // Expect file size or error
             char line[128];
             int r = recv_line(sock, line, sizeof(line));
             if (r <= 0) {
                 fprintf(stderr, "Connection closed by server\n");
                 break;
             }
             if (strncmp(line, "ERROR", 5) == 0) {
                 // If server says ERROR, just print it out
                 printf("%s", line);
                 continue;
             }
             long size = atol(line);
             if (size < 0) {
                 printf("ERROR: Download failed\n");
                 continue;
             }
             // The file name to save locally is everything after the last slash in path
             char *name = strrchr(path, '/');
             name = (name ? name + 1 : path);  // if slash found, skip it; else use path directly
 
             // Create local file
             FILE *fp = fopen(name, "wb");
             if (!fp) {
                 perror("fopen");
                 // If we can't open the file to write, we need to drain the data from the socket
                 long remaining = size;
                 while (remaining > 0) {
                     char discard[512];
                     ssize_t n = recv(sock, discard, (remaining < (long)sizeof(discard) ? remaining : sizeof(discard)), 0);
                     if (n <= 0) break;
                     remaining -= n;
                 }
                 continue;
             }
             // Receive file content
             long remaining = size;
             while (remaining > 0) {
                 char dataBuf[BUF_SIZE];
                 ssize_t n = recv(sock, dataBuf, (remaining < BUF_SIZE ? remaining : BUF_SIZE), 0);
                 if (n <= 0) {
                     break;
                 }
                 fwrite(dataBuf, 1, n, fp);
                 remaining -= n;
             }
             fclose(fp);
             if (remaining == 0) {
                 printf("File %s downloaded (%ld bytes)\n", name, size);
             } else {
                 printf("ERROR: Incomplete download\n");
             }
 
         // --------------- removef ---------------
         } else if (strcmp(cmd, "removef") == 0) {
             char *path = strtok(NULL, "");
             if (!path) {
                 fprintf(stderr, "Usage: removef <file_path_in_S1>\n");
                 continue;
             }
             while (*path == ' ') {
                 path++;
             }
             if (strlen(path) == 0) {
                 fprintf(stderr, "Usage: removef <file_path_in_S1>\n");
                 continue;
             }
             const char *ext = strrchr(path, '.');
             if (!ext || !(strcmp(ext, ".c") == 0 || strcmp(ext, ".pdf") == 0 ||
                           strcmp(ext, ".txt") == 0 || strcmp(ext, ".zip"))) {
                 fprintf(stderr, "Error: unsupported file type for removef\n");
                 continue;
             }
             if (strncmp(path, "~S1", 3) != 0) {
                 fprintf(stderr, "Error: file path must begin with ~S1\n");
                 continue;
             }
             char msg[1024];
             snprintf(msg, sizeof(msg), "removef %s\n", path);
             if (send_all(sock, msg, strlen(msg)) != 0) {
                 fprintf(stderr, "Failed to send 'removef' command\n");
                 continue;
             }
             // Receive response
             char resp[256];
             int r = recv_line(sock, resp, sizeof(resp));
             if (r <= 0) {
                 fprintf(stderr, "Connection closed by server\n");
                 break;
             }
             printf("%s", resp);
 
         // --------------- downltar ---------------
         } else if (strcmp(cmd, "downltar") == 0) {
             char *filetype = strtok(NULL, "");
             if (!filetype) {
                 fprintf(stderr, "Usage: downltar <filetype>\n");
                 continue;
             }
             // Trim spaces
             while (*filetype == ' ') {
                 filetype++;
             }
             if (strlen(filetype) == 0) {
                 fprintf(stderr, "Usage: downltar <filetype>\n");
                 continue;
             }
             // Ensure it starts with a '.' if not provided
             if (filetype[0] != '.') {
                 static char temp[8];
                 snprintf(temp, sizeof(temp), ".%s", filetype);
                 filetype = temp;
             }
             // Must be .c, .pdf, or .txt
             if (!(strcmp(filetype, ".c") == 0 || strcmp(filetype, ".pdf") == 0 || strcmp(filetype, ".txt") == 0)) {
                 fprintf(stderr, "Error: filetype must be .c, .pdf, or .txt\n");
                 continue;
             }
             // Send command
             char msg[32];
             snprintf(msg, sizeof(msg), "downltar %s\n", filetype);
             if (send_all(sock, msg, strlen(msg)) != 0) {
                 fprintf(stderr, "Failed to send 'downltar' command\n");
                 continue;
             }
             // Receive tar size or error
             char line[128];
             int r = recv_line(sock, line, sizeof(line));
             if (r <= 0) {
                 fprintf(stderr, "Connection closed by server\n");
                 break;
             }
             if (strncmp(line, "ERROR", 5) == 0) {
                 printf("%s", line);
                 continue;
             }
             long tarSize = atol(line);
             if (tarSize < 0) {
                 printf("ERROR: Tar creation failed\n");
                 continue;
             }
             // Determine local filename to save the tar
             char outName[32];
             if (strcmp(filetype, ".c") == 0) {
                 strcpy(outName, "cfiles.tar");
             } else if (strcmp(filetype, ".pdf") == 0) {
                 strcpy(outName, "pdf.tar");
             } else if (strcmp(filetype, ".txt") == 0) {
                 strcpy(outName, "text.tar");
             } else {
                 // fallback name
                 strcpy(outName, "output.tar");
             }
             FILE *fp = fopen(outName, "wb");
             if (!fp) {
                 perror("fopen");
                 // If we can't write locally, we still need to drain the data
                 long rem = tarSize;
                 while (rem > 0) {
                     char discard[512];
                     ssize_t n = recv(sock, discard, (rem < (long)sizeof(discard) ? rem : sizeof(discard)), 0);
                     if (n <= 0) break;
                     rem -= n;
                 }
                 continue;
             }
             // Receive the tar file
             long remaining = tarSize;
             while (remaining > 0) {
                 char dataBuf[BUF_SIZE];
                 ssize_t n = recv(sock, dataBuf, (remaining < BUF_SIZE ? remaining : BUF_SIZE), 0);
                 if (n <= 0) {
                     break;
                 }
                 fwrite(dataBuf, 1, n, fp);
                 remaining -= n;
             }
             fclose(fp);
             if (remaining == 0) {
                 printf("Tar file saved as %s \n", outName);
             } else {
                 printf("ERROR: Incomplete tar download\n");
             }
 
         // --------------- dispfnames ---------------
         } else if (strcmp(cmd, "dispfnames") == 0) {
             char *path = strtok(NULL, "");
             if (!path) {
                 fprintf(stderr, "Usage: dispfnames <directory_path_in_S1>\n");
                 continue;
             }
             while (*path == ' ') {
                 path++;
             }
             if (strlen(path) == 0) {
                 fprintf(stderr, "Usage: dispfnames <directory_path_in_S1>\n");
                 continue;
             }
             // We expect a directory, not a file with extension
             char *dot = strrchr(path, '.');
             if (dot) {
                 // If there's an extension, warn user
                 if (strcmp(dot, ".c") == 0 || strcmp(dot, ".pdf") == 0 ||
                     strcmp(dot, ".txt") == 0 || strcmp(dot, ".zip") == 0) {
                     fprintf(stderr, "Error: dispfnames expects a directory, not a file\n");
                     continue;
                 }
             }
             if (strncmp(path, "~S1", 3) != 0) {
                 fprintf(stderr, "Error: directory path must begin with ~S1\n");
                 continue;
             }
             // Send dispfnames command
             char msg[1024];
             snprintf(msg, sizeof(msg), "dispfnames %s\n", path);
             if (send_all(sock, msg, strlen(msg)) != 0) {
                 fprintf(stderr, "Failed to send 'dispfnames' command\n");
                 continue;
             }
             // Possible responses:
             //   1) "No files found\n"
             //   2) "ERROR: ...
             //   3) A line with a numeric length, then that many bytes of filenames
             char line[128];
             int r = recv_line(sock, line, sizeof(line));
             if (r <= 0) {
                 fprintf(stderr, "Connection closed by server\n");
                 break;
             }
             if (strncmp(line, "ERROR", 5) == 0 || strncmp(line, "No files found", 14) == 0) {
                 // Just print the line as is
                 printf("%s", line);
                 continue;
             }
             // Otherwise, line should be a length
             long listSize = atol(line);
             if (listSize < 0) {
                 printf("ERROR: Failed to retrieve file list\n");
                 continue;
             }
             if (listSize == 0) {
                 printf("No files found\n");
                 continue;
             }
             // Allocate buffer for list
             char *listBuf = (char*)malloc(listSize + 1);
             if (!listBuf) {
                 fprintf(stderr, "Memory allocation error\n");
                 // Drain data from the socket
                 long rem = listSize;
                 while (rem > 0) {
                     char discard[256];
                     ssize_t n = recv(sock, discard, (rem < (long)sizeof(discard) ? rem : sizeof(discard)), 0);
                     if (n <= 0) break;
                     rem -= n;
                 }
                 continue;
             }
             if (recv_all(sock, listBuf, listSize) != 0) {
                 fprintf(stderr, "Failed to receive file list\n");
                 free(listBuf);
                 continue;
             }
             listBuf[listSize] = '\0';
             // Print the file list
             printf("%s", listBuf);
             free(listBuf);
 
         // --------------- unknown command ---------------
         } else {
             fprintf(stderr, "Unknown command: %s\n", cmd);
             fprintf(stderr, "Commands: uploadf, downlf, removef, downltar, dispfnames, quit\n");
         }
     }
 
     // End of loop, close socket
     close(sock);
     printf("Client disconnected.\n");
     return 0;
 }
 