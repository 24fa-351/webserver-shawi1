#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>

#define BUFFER_SIZE 2048
#define STANDARD_PORT 80

// Global statistics
int total_requests = 0;
int total_bytes_in = 0;
int total_bytes_out = 0;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

int initialize_server(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(port);

    int option = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    if (bind(sock, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        perror("Binding failed");
        exit(EXIT_FAILURE);
    }

    if (listen(sock, 10) < 0)
    {
        perror("Listening failed");
        exit(EXIT_FAILURE);
    }

    return sock;
}

void decode_request(const char *request_data, char *method_buffer, char *url_buffer)
{
    sscanf(request_data, "%s %s", method_buffer, url_buffer);
}

void respond_to_client(int socket_fd, const char *status, const char *content_type, const char *content)
{
    char response[BUFFER_SIZE];
    int response_len = snprintf(response, BUFFER_SIZE, "%s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n%s", status, content_type, strlen(content), content);
    write(socket_fd, response, response_len);

    pthread_mutex_lock(&stats_mutex);
    total_bytes_out += response_len;
    pthread_mutex_unlock(&stats_mutex);
}

void display_stats(int socket_fd)
{
    char response_body[1024];
    snprintf(response_body, sizeof(response_body), "<html><body><h1>Server Statistics</h1><p>Total Requests: %d</p><p>Total Bytes Received: %d</p><p>Total Bytes Sent: %d</p></body></html>", total_requests, total_bytes_in, total_bytes_out);
    respond_to_client(socket_fd, "HTTP/1.1 200 OK", "text/html", response_body);
}

void calculate_and_respond(int socket_fd, const char *query_string)
{
    char *parameters = strchr(query_string, '?');
    int num1 = 0, num2 = 0;

    if (parameters)
    {
        sscanf(parameters, "?a=%d&b=%d", &num1, &num2);
    }

    int sum = num1 + num2;
    char result[1024];
    snprintf(result, sizeof(result), "<html><body><h1>Calculation Result</h1><p>%d + %d = %d</p></body></html>", num1, num2, sum);
    respond_to_client(socket_fd, "HTTP/1.1 200 OK", "text/html", result);
}

void deliver_static_content(int socket_fd, const char *file_path)
{
    char path[1024];
    // Ensure the path points to the "static" directory correctly
    snprintf(path, sizeof(path), "./static%s", file_path); // Assuming your server executable is in the root directory

    int file = open(path, O_RDONLY);
    struct stat file_stats;

    if (file < 0 || fstat(file, &file_stats) < 0)
    {
        respond_to_client(socket_fd, "HTTP/1.1 404 Not Found", "text/html", "<html><body><h1>File Not Found</h1></body></html>");
    }
    else
    {
        char *file_content = malloc(file_stats.st_size);
        read(file, file_content, file_stats.st_size);
        char header[1024];
        sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %ld\r\n\r\n", file_stats.st_size);
        write(socket_fd, header, strlen(header));
        write(socket_fd, file_content, file_stats.st_size);

        pthread_mutex_lock(&stats_mutex);
        total_bytes_out += strlen(header) + file_stats.st_size;
        pthread_mutex_unlock(&stats_mutex);

        free(file_content);
    }
    close(file);
}

void *client_session(void *socket_descriptor)
{
    int socket_fd = (intptr_t)socket_descriptor;
    char buffer[BUFFER_SIZE], method[10], url[1024];
    int bytes_read = read(socket_fd, buffer, BUFFER_SIZE - 1);

    if (bytes_read > 0)
    {
        buffer[bytes_read] = '\0';

        pthread_mutex_lock(&stats_mutex);
        total_bytes_in += strlen(buffer);
        total_requests++;
        pthread_mutex_unlock(&stats_mutex);

        decode_request(buffer, method, url);

        if (strncmp(url, "/static/", 8) == 0)
        {
            deliver_static_content(socket_fd, url + 7);
        }
        else if (strcmp(url, "/stats") == 0)
        {
            display_stats(socket_fd);
        }
        else if (strncmp(url, "/calc", 5) == 0)
        {
            calculate_and_respond(socket_fd, url);
        }
        else
        {
            respond_to_client(socket_fd, "HTTP/1.1 404 Not Found", "text/html", "<html><body><h1>404 Not Found</h1></body></html>");
        }
    }

    close(socket_fd);
    return NULL;
}

int main(int argc, char **argv)
{
    int port = STANDARD_PORT, opt;
    while ((opt = getopt(argc, argv, "p:")) != -1)
    {
        if (opt == 'p')
            port = atoi(optarg);
        else
        {
            fprintf(stderr, "Usage: %s [-p port_number]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    int server_socket = initialize_server(port);
    printf("Listening on port %d\n", port);

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket < 0)
        {
            perror("Failed to accept connection");
            continue;
        }

        pthread_t new_thread;
        if (pthread_create(&new_thread, NULL, client_session, (void *)(intptr_t)client_socket) != 0)
        {
            perror("Could not create thread");
            close(client_socket);
        }
        else
        {
            pthread_detach(new_thread);
        }
    }

    close(server_socket);
    return 0;
}