# Linux_Distributed_File_System_Project
A simple distributed file system in C using UNIX sockets. Clients interact with a single main server (S1), which transparently routes files by type to dedicated back‑end servers (S2–S4).

---

## Table of Contents

1. [Overview](#overview)  
2. [Architecture](#architecture)  
3. [Features](#features)  
4. [Getting Started](#getting-started)  
   - [Prerequisites](#prerequisites)  
   - [Directory Setup](#directory-setup)  
   - [Build](#build)  
   - [Run](#run)  
5. [Usage Examples](#usage-examples)  
6. [Learning Outcomes](#learning-outcomes)  
7. [License](#license)

---

## Overview

This project demonstrates core systems programming concepts by implementing:

- **S1 (Main Server):** Forks a child for each client, stores `.c` files locally, and forwards other file types to S2–S4.  
- **S2, S3, S4 (Back‑End Servers):** Each serves a single file type (`.pdf`, `.txt`, `.zip`), handling storage, retrieval, deletion, and listing.  
- **w25clients (Client):** Command‑line interface to upload, download, and manage files via S1.

---

## Architecture
## Architecture

Below is the high‑level architecture of the distributed file system:

![Distributed File System Architecture](assets/architecture.png)

<small>**Figure 1.** Client (`w25clients`) → Main Server (`S1`) → PDF Server (`S2`), Text Server (`S3`), ZIP Server (`S4`). Ports shown are defaults.</small>

