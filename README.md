# Asynchronous Web Server (AWS)

A high-performance, single-threaded web server implemented in C for Linux environments. This project focuses on utilizing advanced Linux I/O operations to serve HTTP requests efficiently without the overhead of multi-threading or multi-processing. 

The core design principle is that the main thread must never block. It achieves this through a combination of I/O multiplexing, non-blocking sockets, zero-copy data transfer, and Linux native asynchronous I/O (AIO).

---

## Architecture and Implementation Details

The server operates entirely within a single thread, driven by an event loop. Each connected client is tracked using a custom state machine that dictates the next non-blocking action to perform when a socket becomes ready.

### 1. The Event Loop (epoll)
The heart of the server is an `epoll(7)` instance. Instead of spawning threads for incoming clients, the server adds the listening socket and all client sockets to the `epoll` instance. The main loop calls `epoll_wait()`, suspending execution until one or more file descriptors are ready for reading or writing.



### 2. Connection State Machine
To manage concurrent clients without blocking, every connection is wrapped in a `connection` structure containing a deterministic state machine. The primary states include:

* **STATE_RECEIVING_DATA:** Reading the incoming HTTP request into a local buffer.
* **STATE_REQUEST_RECEIVED:** Parsing the request to extract the file path.
* **STATE_SENDING_HEADER:** Writing the `HTTP 200 OK` or `HTTP 404 Not Found` headers to the socket.
* **STATE_SENDING_DATA:** Streaming the actual file content to the client.
* **STATE_ASYNC_ONGOING:** Waiting for the disk to finish reading a chunk of data.

If a socket read or write returns `EAGAIN` or `EWOULDBLOCK`, the state machine simply pauses, returning control to the `epoll` loop until the socket is ready again.

### 3. Serving Static Content (Zero-Copy)
When a client requests a file from the `static/` directory, the server bypasses standard user-space memory buffers. 

It uses the `sendfile(2)` system call to instruct the kernel to copy data directly from the filesystem's page cache to the network socket's buffer. This "zero-copy" approach drastically reduces CPU usage and memory context switches.



### 4. Serving Dynamic Content (Asynchronous I/O)
For files requested from the `dynamic/` directory, the server demonstrates non-blocking disk reads using the Linux native AIO API (`libaio`):

1.  **Submission:** The server prepares an I/O Control Block (IOCB) and submits the read request to the kernel using `io_submit(2)`.
2.  **Notification:** To avoid blocking while waiting for the disk, the IOCB is linked to an `eventfd` file descriptor. This `eventfd` is registered with the main `epoll` loop.
3.  **Completion:** When the disk finishes reading the chunk into the connection's buffer, the kernel signals the `eventfd`. The `epoll` loop wakes up, processes the data using `io_getevents(2)`, and transitions the state machine to send the buffer to the client.

## Build and Run Instructions

### Prerequisites
* Linux environment (relies strictly on Linux-specific syscalls like `epoll` and `sendfile`)
* GCC and Make
* `libaio-dev` package (`sudo apt-get install libaio-dev` on Debian/Ubuntu)

### Compilation
Navigate to the root directory or the `src/` directory and run `make` to build the `aws` executable.

### Credits
Implementation Logic: Constantin (CostelXD)
Framework & Infrastructure: Razvan Deaconescu & Liza Babu

