/*
 * COMP 321 Project 6: Web Proxy
 *
 * This program implements a multithreaded HTTP proxy.
 *
 * <Jialiang Yang jy105; Sunny Sun ss303>
 */

#include <assert.h>
#include <pthread.h>

#include "csapp.h"

#define NTHREADS 8  /* Number of worker threads */
#define SBUFSIZE 16 /* Size of the shared buffer */

typedef struct {
	struct sockaddr_in client_addr; /* Client address */
	int n;				/* Request size */
	int connfd; /* File descriptor for the socket connection to the client
		     */
	char *host; /* Pointer to a string holding the host name */
	char *port; /* Pointer to a string holding the port number */
} sbuf_t;

FILE *log_file; /* Global file pointer for logging server requests */

/* Index variables for managing the buffer */
int idx_producer; /* Index where the producer will insert the next item in the
		     buffer */
int idx_consumer; /* Index where the consumer will remove the next item from the
		     buffer */
int num_shared;	  /* Number of items currently shared in the buffer */
int num_curr;	  /* Number of total items in the buffer */

/* Mutexes for synchronizing access to shared resources among threads */
pthread_mutex_t thread_mutex; /* Mutex for thread synchronization */
pthread_mutex_t
    log_mutex; /* Mutex specifically for synchronizing access to the log file */

/* Condition variables for managing producer-consumer states */
pthread_cond_t
    cond_empty; /* Condition variable to signal that the buffer has space */
pthread_cond_t
    cond_full; /* Condition variable to signal that the buffer has items */

sbuf_t *sbuf[SBUFSIZE]; /* A buffer for storing client requests */

/* Function prototypes */
void *thread(void *vargp);
void sbuf_init();
void sbuf_deinit();
void process_client_request(int fd, struct sockaddr_in *clientaddrp,
    unsigned int number);
void log_transaction(char *log_entry, FILE *file);
static void client_error(int fd, const char *cause, int err_num,
    const char *short_msg, const char *long_msg);
static char *create_log_entry(const struct sockaddr_in *sockaddr,
    const char *uri, int size);
static int parse_uri(const char *uri, char **hostnamep, char **portp,
    char **pathnamep);

/*
 * Requires:
 *   Nothing.
 *
 * Effects:
 *   Initializes two mutexes: thread_mutex for general thread synchronization,
 *   and log_mutex for protecting log file accesses; initializes two condition
 *   variables: cond_empty to signal producers that the buffer is not full and
 *   cond_full to signal consumers that the buffer contains items.
 */
void
sbuf_init()
{
	Pthread_mutex_init(&thread_mutex, NULL);
	Pthread_mutex_init(&log_mutex, NULL);
	Pthread_cond_init(&cond_empty, NULL);
	Pthread_cond_init(&cond_full, NULL);
}

/*
 * Requires:
 *   Nothing.
 *
 * Effects:
 *   Destroys thread_mutex and log_mutex; destroys cond_empty and cond_full.
 */
void
sbuf_deinit()
{
	Pthread_mutex_destroy(&thread_mutex);
	Pthread_mutex_destroy(&log_mutex);
	Pthread_cond_destroy(&cond_empty);
	Pthread_cond_destroy(&cond_full);
}

/*
 * Requires:
 *   Nothing.
 *
 * Effects:
 *   Continuously processes client requests from a shared circular buffer. For
 *   each request, the function handles synchronization to safely fetch the
 *   request data, processes the request, and then signals that space is
 *   available in the buffer.
 */
void *
thread(void *vargp)
{
	(void)vargp;
	// Detach the thread to allow independent operation
	Pthread_detach(pthread_self());

	sbuf_t *buf; // Pointer to buffer item type
	while (1) {
		pthread_mutex_lock(&thread_mutex);
		// Wait for the buffer to contain items
		while (num_shared == 0) {
			Pthread_cond_wait(&cond_full, &thread_mutex);
		}
		// Fetch the next item to process
		buf = sbuf[idx_consumer];
		// Update the consumer index in a circular manner
		if (idx_consumer == SBUFSIZE - 1)
			idx_consumer = 0;
		else
			idx_consumer++;

		// Decrease the count of items in the buffer
		num_shared -= 1;
		Pthread_mutex_unlock(&thread_mutex);
		// Signal to potential producers that there is now space
		// available
		Pthread_cond_broadcast(&cond_empty);

		process_client_request(buf->connfd, &buf->client_addr, buf->n);
		Close(buf->connfd);
	}
	return (NULL);
}

/*
 * Requires:
 *   - "conn_fd" must be a valid and open socket file descriptor for
 * communicating with a client.
 *   - "clientaddr" must point to a valid 'sockaddr_in' structure containing the
 * client's address information.
 *   - "number" should be a unique identifier for the request.
 *
 * Effects:
 *   Reads the HTTP request from the client connected. Parses the request and
 *   determines the method, URI, and HTTP version. Receives the response from
 *   the server and forwards it back to the client. Logs the transaction details
 *   including the client's IP, requested URI, and response size. Properly
 *   handles and logs various errors such as DNS resolution failures, connection
 *   issues, and invalid requests. Closes the connection to the client and frees
 *   allocated resources upon completion of the request handling.
 */
void
process_client_request(int conn_fd, struct sockaddr_in *clientaddr,
    unsigned int number)
{
	char method[MAXLINE], uri[MAXLINE], version[MAXLINE], header[MAXLINE];
	char haddrp[INET_ADDRSTRLEN];
	rio_t rio_client, rio_server;
	char *hostname, *port, *pathname;
	int connfd = conn_fd;
	int n = 0, i = 0, size = 0, num_buf = 1;
	int len_header;

	// Initialize robust I/O for the client connection
	rio_readinitb(&rio_client, conn_fd);

	// Allocate buffer for reading request
	char *buf = Malloc(MAXLINE);
	if (buf == NULL) {
		client_error(connfd, "Server Error", 500,
		    "Internal Server Error", "Memory allocation failed");
	}

	// Read the request header line by line until '\r\n'
	while ((n = rio_readlineb(&rio_client, &buf[i], MAXLINE)) > 0) {
		i += n;
		if (buf[i - 1] == '\n')
			break; // Properly handle the end of line
		if (i == num_buf * MAXLINE - 1) { // Buffer is full
			num_buf *= 2; // Increase the buffer size exponentially
			char *new_buf = Realloc(buf, num_buf * MAXLINE);
			if (!new_buf) {
				free(buf);
				fprintf(stderr, "Reallocation failed\n");
				return;
			}
			buf = new_buf;
		}
	}

	// Read the request
	if (n < 0) {
		client_error(connfd, buf, 400, "Bad Request",
		    "Cannot read request");
		return;
	}

	// Parse the request
	if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
		client_error(connfd, buf, 400, "Bad Request",
		    "Cannot parse request");
		return;
	}

	// Verify HTTP version support
	if (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")) {
		client_error(connfd, version, 505, "HTTP Version Not Supported",
		    "Invalid HTTP version");
		return;
	}

	// Check if the method is GET
	if (strcasecmp(method, "GET") != 0) {
		client_error(connfd, method, 501, "Not Implemented",
		    "This method is not implemented");
		return;
	}

	// Parse the URI
	if (parse_uri(uri, &hostname, &port, &pathname) == -1) {
		client_error(connfd, uri, 400, "Bad Request", "Malformed URI");
		return;
	}

	// Construct the forward header
	len_header = strlen(method) + 1 + strlen(pathname) + 1 +
	    strlen(version) + 2 + 1;
	if (len_header >= MAXLINE) {
		client_error(connfd, "Header too long", 500,
		    "Internal Server Error", "Header rewrite failed");
		return;
	}
	snprintf(header, len_header, "%s %s %s\r\n", method, pathname, version);

	// Print details about the request
	Inet_ntop(AF_INET, &(clientaddr->sin_addr), haddrp, INET_ADDRSTRLEN);
	printf("Request %u: Received request from client (%s):\n", number,
	    haddrp);
	printf("%s\n\n", header);

	// Try to connect to the server
	int clientfd = open_clientfd(hostname, port);
	if (clientfd < 0) {
		if (errno == ENOENT) {
			client_error(connfd, hostname, 503,
			    "Service Unavailable", "DNS resolution failed");
		} else {
			client_error(connfd, hostname, 502, "Bad Gateway",
			    "Could not connect to server");
		}
		return;
	}

	rio_readinitb(&rio_server, clientfd);

	// Forward the header to the server
	if ((rio_writen(clientfd, header, strlen(header))) < 0) {
		client_error(clientfd, "Forward Header", 500,
		    "Internal Server Error", "Failed to send header to server");
		return;
	}

	printf("\n");
	printf("*** End of Request ***\n");
	printf("Request %d: Forwarding request to server:\n", number);
	printf("%.*s", (int)strlen(header), header);

	// Forward all subsequent headers and the body
	while ((n = rio_readlineb(&rio_client, buf, MAXLINE)) > 0) {
		if (strcmp(buf, "\r\n") == 0) {
			rio_writen(clientfd, buf, n);
			break;
		} else if (strncmp(buf, "Connection", 10) == 0) {
			printf("Request %d: Stripping \"Connection\" header\n",
			    number);
			continue;
		} else if (strncmp(buf, "Keep-Alive", 10) == 0) {
			printf("Request %d: Stripping \"Keep-Alive\" header\n",
			    number);
			continue;
		} else if (strncmp(buf, "Proxy-Connection", 16) == 0) {
			printf(
			    "Request %d: Stripping \"Proxy-connection\" header\n",
			    number);
			continue;
		} else {
			printf("%s", buf);
			if ((rio_writen(clientfd, buf, n)) < 0) {
				client_error(clientfd, buf, 400, "Bad request",
				    "Cannot store headers");
				return;
			}
		}
	}

	if (strstr(version, "HTTP/1.1")) {
		printf("Connection: close\n");
	}

	// Close the header section and forward the body
	rio_writen(clientfd, "Connection: close\r\n", 19);
	rio_writen(clientfd, "\r\n", 2);

	// Read the response
	while ((n = rio_readnb(&rio_server, buf, MAXLINE)) != 0) {
		size += n;
		if ((rio_writen(connfd, buf, n)) < 0) {
			client_error(clientfd, buf, 400, "Bad request",
			    "No server response");
			return;
		}
	}

	printf("\n");
	printf("*** End of Request ***\n");
	printf("Request %d: Forwarded %d bytes from server to client\n", number,
	    size);

	// Log the transaction
	char *log_entry = create_log_entry(clientaddr, uri, size);
	log_transaction(log_entry, log_file);
	// Clean up
	free(log_entry);
	free(hostname);
	free(port);
	free(pathname);
	free(buf);
	Close(clientfd);
}

/*
 * Requires:
 *   - "log_entry" must be a null-terminated string that contains the formatted
 * log information to be written to the log file. It should not be NULL.
 *   - "file" must be a valid, open a file that is writable. The file should
 * have been opened previously, and it must not be NULL.
 *
 * Effects:
 *   Writes "log_entry" to "file", appending a newline character to maintain
 *   proper log formatting. Ensures thread-safe writing by using a mutex to
 *   synchronize access to the 'file'. Flushes the file buffer to ensure the
 *   entry is written to disk immediately.
 */
void
log_transaction(char *log_entry, FILE *file)
{
	pthread_mutex_lock(&log_mutex);
	// Append a newline character to separate log entries visually
	strcat(log_entry, "\n");
	// Write the log entry to the file
	fwrite(log_entry, sizeof(char), strlen(log_entry), file);
	// Flush the file stream to ensure all buffered data is written to the
	// disk
	fflush(file);
	pthread_mutex_unlock(&log_mutex);
}

/*
 * Requires:
 *   "argc" should be exactly 2, ensuring the command line includes the
 *   executable name and one argument representing the port number. "argv"
 *   should be a valid pointer to the array of command-line arguments.
 *
 * Effects:
 *   Initializes synchronization primitives and the shared buffer, handles
 *   SIGPIPE signals, opens the logging file, and creates a listening socket on
 *   the specified port. Spawns a fixed number of worker threads to process
 *   incoming client connections concurrently. Each connection is accepted in
 *   the main loop, where a new buffer item is allocated and initialized with
 *   connection details, then passed to the worker threads. Continuously accepts
 *   connections until the server shuts down, at which point resources are
 *   cleaned up and the program exits.
 */
int
main(int argc, char **argv)
{
	struct sockaddr_in clientaddr;
	socklen_t clientlen = sizeof(struct sockaddr_in);
	int connfd, listenfd, i;  // File descriptors for client and listen
				  // socket, and loop counter
	pthread_t tids[NTHREADS]; // Array for thread IDs

	// Check if command line argument count is correct
	if (argc != 2) {
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(0);
	}

	// Initialize synchronization mechanisms and shared buffer
	sbuf_init();

	// Ignore SIGPIPE to handle writes to connections that have been closed
	// by the client
	Signal(SIGPIPE, SIG_IGN);

	// Open the file
	if ((log_file = fopen("proxy.log", "a")) == NULL) {
		perror("Failed to open a file");
		exit(0);
	}

	// Open a listening socket on the specified port
	listenfd = Open_listenfd(argv[1]);
	if (listenfd < 0) {
		fprintf(stderr, "Error opening listen socket\n");
	}

	// Create worker threads to handle requests
	for (i = 0; i < NTHREADS; i++) {
		Pthread_create(&tids[i], NULL, thread, NULL);
	}

	// Main server loop: accept and handle requests
	while (1) {
		sbuf_t *buf = Malloc(sizeof(sbuf_t));
		char host_name[MAXLINE], port[MAXLINE];
		// Accept a client connection
		connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
		Getnameinfo((SA *)&clientaddr, clientlen, host_name, MAXLINE,
		    port, MAXLINE, 0);

		// Initialize buffer item with connection details
		buf->connfd = connfd;
		buf->client_addr = clientaddr;
		buf->host = host_name;
		buf->port = port;
		buf->n = num_curr;

		// Lock the thread mutex to protect shared data
		Pthread_mutex_lock(&thread_mutex);

		// Wait until there is space in the buffer
		while (num_shared == SBUFSIZE) {
			Pthread_cond_wait(&cond_empty, &thread_mutex);
		}

		// Update shared buffer and indices
		num_curr++;
		sbuf[idx_producer] = buf;
		Pthread_cond_broadcast(&cond_full);

		// Update producer index
		if (idx_producer == SBUFSIZE - 1)
			idx_producer = 0;
		else
			idx_producer++;
		num_shared++;
		Pthread_mutex_unlock(&thread_mutex);
	}
	// Clean up
	sbuf_deinit();
	Fclose(log_file);
	Close(listenfd);

	/* Return success. */
	return (0);
}

/*
 * Requires:
 *   The parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Given a URI from an HTTP proxy GET request (i.e., a URL), extract the
 *   host name, port, and path name.  Create strings containing the host name,
 *   port, and path name, and return them through the parameters "hostnamep",
 *   "portp", "pathnamep", respectively.  (The caller must free the memory
 *   storing these strings.)  Return -1 if there are any problems and 0
 *   otherwise.
 */
static int
parse_uri(const char *uri, char **hostnamep, char **portp, char **pathnamep)
{
	const char *pathname_begin, *port_begin, *port_end;

	if (strncasecmp(uri, "http://", 7) != 0)
		return (-1);

	/* Extract the host name. */
	const char *host_begin = uri + 7;
	const char *host_end = strpbrk(host_begin, ":/ \r\n");
	if (host_end == NULL)
		host_end = host_begin + strlen(host_begin);
	int len = host_end - host_begin;
	char *hostname = Malloc(len + 1);
	strncpy(hostname, host_begin, len);
	hostname[len] = '\0';
	*hostnamep = hostname;

	/* Look for a port number.  If none is found, use port 80. */
	if (*host_end == ':') {
		port_begin = host_end + 1;
		port_end = strpbrk(port_begin, "/ \r\n");
		if (port_end == NULL)
			port_end = port_begin + strlen(port_begin);
		len = port_end - port_begin;
	} else {
		port_begin = "80";
		port_end = host_end;
		len = 2;
	}
	char *port = Malloc(len + 1);
	strncpy(port, port_begin, len);
	port[len] = '\0';
	*portp = port;

	/* Extract the path. */
	if (*port_end == '/') {
		pathname_begin = port_end;
		const char *pathname_end = strpbrk(pathname_begin, " \r\n");
		if (pathname_end == NULL)
			pathname_end = pathname_begin + strlen(pathname_begin);
		len = pathname_end - pathname_begin;
	} else {
		pathname_begin = "/";
		len = 1;
	}
	char *pathname = Malloc(len + 1);
	strncpy(pathname, pathname_begin, len);
	pathname[len] = '\0';
	*pathnamep = pathname;

	return (0);
}

/*
 * Requires:
 *   The parameter "sockaddr" must point to a valid sockaddr_in structure.  The
 *   parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Returns a string containing a properly formatted log entry.  This log
 *   entry is based upon the socket address of the requesting client
 *   ("sockaddr"), the URI from the request ("uri"), and the size in bytes of
 *   the response from the server ("size").
 */
static char *
create_log_entry(const struct sockaddr_in *sockaddr, const char *uri, int size)
{
	struct tm result;

	/*
	 * Create a large enough array of characters to store a log entry.
	 * Although the length of the URI can exceed MAXLINE, the combined
	 * lengths of the other fields and separators cannot.
	 */
	const size_t log_maxlen = MAXLINE + strlen(uri);
	char *const log_str = Malloc(log_maxlen + 1);

	/* Get a formatted time string. */
	time_t now = time(NULL);
	int log_strlen = strftime(log_str, MAXLINE,
	    "%a %d %b %Y %H:%M:%S %Z: ", localtime_r(&now, &result));

	/*
	 * Convert the IP address in network byte order to dotted decimal
	 * form.
	 */
	Inet_ntop(AF_INET, &sockaddr->sin_addr, &log_str[log_strlen],
	    INET_ADDRSTRLEN);
	log_strlen += strlen(&log_str[log_strlen]);

	/*
	 * Assert that the time and IP address fields occupy less than half of
	 * the space that is reserved for the non-URI fields.
	 */
	assert(log_strlen < MAXLINE / 2);

	/*
	 * Add the URI and response size onto the end of the log entry.
	 */
	snprintf(&log_str[log_strlen], log_maxlen - log_strlen, " %s %d", uri,
	    size);

	return (log_str);
}

/*
 * Requires:
 *   The parameter "fd" must be an open socket that is connected to the client.
 *   The parameters "cause", "short_msg", and "long_msg" must point to properly
 *   NUL-terminated strings that describe the reason why the HTTP transaction
 *   failed.  The string "short_msg" may not exceed 32 characters in length,
 *   and the string "long_msg" may not exceed 80 characters in length.
 *
 * Effects:
 *   Constructs an HTML page describing the reason why the HTTP transaction
 *   failed, and writes an HTTP/1.0 response containing that page as the
 *   content.  The cause appearing in the HTML page is truncated if the
 *   string "cause" exceeds 2048 characters in length.
 */
static void
client_error(int fd, const char *cause, int err_num, const char *short_msg,
    const char *long_msg)
{
	char body[MAXBUF], headers[MAXBUF], truncated_cause[2049];

	assert(strlen(short_msg) <= 32);
	assert(strlen(long_msg) <= 80);
	/* Ensure that "body" is much larger than "truncated_cause". */
	assert(sizeof(truncated_cause) < MAXBUF / 2);

	/*
	 * Create a truncated "cause" string so that the response body will not
	 * exceed MAXBUF.
	 */
	strncpy(truncated_cause, cause, sizeof(truncated_cause) - 1);
	truncated_cause[sizeof(truncated_cause) - 1] = '\0';

	/* Build the HTTP response body. */
	snprintf(body, MAXBUF,
	    "<html><title>Proxy Error</title><body bgcolor=\"ffffff\">\r\n"
	    "%d: %s\r\n"
	    "<p>%s: %s\r\n"
	    "<hr><em>The COMP 321 Web proxy</em>\r\n",
	    err_num, short_msg, long_msg, truncated_cause);

	/* Build the HTTP response headers. */
	snprintf(headers, MAXBUF,
	    "HTTP/1.0 %d %s\r\n"
	    "Content-type: text/html\r\n"
	    "Content-length: %d\r\n"
	    "\r\n",
	    err_num, short_msg, (int)strlen(body));

	/* Write the HTTP response. */
	if (rio_writen(fd, headers, strlen(headers)) != -1)
		rio_writen(fd, body, strlen(body));
}

// Prevent "unused function" and "unused variable" warnings.
static const void *dummy_ref[] = { client_error, create_log_entry, dummy_ref,
	parse_uri };