/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   main.c                                             :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: akyoshid <akyoshid@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2025/12/01 13:01:19 by akyoshid          #+#    #+#             */
/*   Updated: 2025/12/01 13:33:19 by akyoshid         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>

///////////////////////////////////////////////////
// Global variables
///////////////////////////////////////////////////
int			g_server_fd;
int			g_max_fd;
fd_set		g_master_fds;

typedef struct s_database {
	int		id;
	char	*buff;
}	t_database;

t_database	g_database[FD_SETSIZE];
int			g_next_id = 0;

///////////////////////////////////////////////////
// Provided functions
///////////////////////////////////////////////////

// split buf to (new) buf & msg
// (old) buf memory will be reused for msg (split by NULL)
// (new)buf & msg should be freed after this function
int	extract_message(char **buf, char **msg)
{
	char	*newbuf;
	int		i;

	*msg = 0;
	if (*buf == 0)
		return (0);
	i = 0;
	while ((*buf)[i])
	{
		if ((*buf)[i] == '\n')
		{
			newbuf = calloc(1, sizeof(*newbuf) * (strlen(*buf + i + 1) + 1));
			if (newbuf == 0)
				return (-1);
			strcpy(newbuf, *buf + i + 1);
			*msg = *buf;
			(*msg)[i + 1] = 0;
			*buf = newbuf;
			return (1);
		}
		i++;
	}
	return (0);
}

// Return new string (buf + add)
// New string must be freed after this function
// buf will be freed by this function
// add will not be freed by this function
// buf can be NULL
// add can't be NULL
char	*str_join(char *buf, char *add)
{
	char	*newbuf;
	int		len;

	if (buf == 0)
		len = 0;
	else
		len = strlen(buf);
	newbuf = malloc(sizeof(*newbuf) * (len + strlen(add) + 1));
	if (newbuf == 0)
		return (0);
	newbuf[0] = 0;
	if (buf != 0)
		strcat(newbuf, buf);
	free(buf);
	strcat(newbuf, add);
	return (newbuf);
}

///////////////////////////////////////////////////
// Helper functions
///////////////////////////////////////////////////
void	print_log(char *str) {
	if (str != NULL)
		write(2, str, strlen(str));
}

void	fatal_error(void) {
	write(2, "Fatal error\n", 12);
	exit(1);
}

void	clean_up(void) {
	for (int fd = 0; fd <= g_max_fd; ++fd) {
		if (FD_ISSET(fd, &g_master_fds) && fd != g_server_fd) {
			FD_CLR(fd, &g_master_fds);
			g_database[fd].id = -1;
			free(g_database[fd].buff);
			g_database[fd].buff = NULL;
			close(fd);
		}
	}
	FD_ZERO(&g_master_fds);
	close(g_server_fd);
}

void	broadcast(int sender_fd, const char *mes) {
	// print_log("broadcast\n");
	for (int fd = 0; fd <= g_max_fd; ++fd) {
		if (FD_ISSET(fd, &g_master_fds) && fd != g_server_fd && fd != sender_fd)
			send(fd, mes, strlen(mes), 0);
	}
}

///////////////////////////////////////////////////
// Connection
///////////////////////////////////////////////////
void	handle_new_connection() {
	// print_log("handle_new_connection\n");
	int new_fd = accept(g_server_fd, NULL, NULL);
	if (new_fd >= 0) {
		// set up
		FD_SET(new_fd, &g_master_fds);
		if (g_max_fd < new_fd)
			g_max_fd = new_fd;
		g_database[new_fd].id = g_next_id;
		++g_next_id;
		g_database[new_fd].buff = NULL;
		// broadcast
		char message[64];
		sprintf(message,
			"server: client %d just arrived\n", g_database[new_fd].id);
		broadcast(new_fd, message);
	}
}

void	handle_disconnection(int fd) {
	// print_log("handle_disconnection\n");
	// broadcast
	char message[64];
	sprintf(message, "server: client %d just left\n", g_database[fd].id);
	broadcast(fd, message);
	// clean up
	FD_CLR(fd, &g_master_fds);
	g_database[fd].id = -1;
	free(g_database[fd].buff);
	g_database[fd].buff = NULL;
	close(fd);
}

///////////////////////////////////////////////////
// Recv data
///////////////////////////////////////////////////
void	recv_data(int recv_fd) {
	// print_log("recv_data\n");
	char data[4096];
	int ret1 = recv(recv_fd, data, sizeof(data) - 1, 0);
	if (ret1 > 0) {
		data[ret1] = '\0';

		// join buff + data
		char *joined_str = str_join(g_database[recv_fd].buff, data);
		if (joined_str == NULL) {
			clean_up();
			fatal_error();
		}
		g_database[recv_fd].buff = joined_str;

		// extract message
		char *message;
		while (1) {
			int ret2 = extract_message(&g_database[recv_fd].buff, &message);
			if (ret2 > 0) {

				char prefix[32];
				sprintf(prefix, "client %d: ", g_database[recv_fd].id);
				char *prefix_alloced = str_join(NULL, prefix);
				if (prefix_alloced == NULL) {
					free(message);
					clean_up();
					fatal_error();
				}

				char *message_with_prefix = str_join(prefix_alloced, message);
				free(message);
				if (message_with_prefix == NULL) {
					free(prefix_alloced);
					clean_up();
					fatal_error();
				}

				broadcast(recv_fd, message_with_prefix);
				free(message_with_prefix);

			} else if (ret2 == 0) {
				break ;
			} else {
				clean_up();
				fatal_error();
			}		
		}
	} else if (ret1 <= 0) {
		handle_disconnection(recv_fd);
	}
}

///////////////////////////////////////////////////
// main
///////////////////////////////////////////////////
int	main(int argc, char **argv) {
	if (argc < 2) {
		write(2, "Wrong number of arguments\n", 26);
		return (1);
	}

	g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (g_server_fd < 0)
		fatal_error();

	struct sockaddr_in addr;
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(atoi(argv[1]));
	if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(g_server_fd);
		fatal_error();
	}

	if (listen(g_server_fd, 128) < 0) {
		close(g_server_fd);
		fatal_error();
	}

	FD_ZERO(&g_master_fds);
	FD_SET(g_server_fd, &g_master_fds);
	g_max_fd = g_server_fd;

	while (1) {
		fd_set read_fds = g_master_fds;
		int ret = select(g_max_fd + 1, &read_fds, NULL, NULL, NULL);
		if (ret > 0) {
			for (int fd = 0; fd <= g_max_fd; ++fd) {
				if (FD_ISSET(fd, &read_fds)) {
					if (fd == g_server_fd)
						handle_new_connection();
					else
						recv_data(fd);
				}
			}
		} else if (ret < 0) {
			if (errno == EINTR)
				continue;
			clean_up();
			fatal_error();
		}
	}
	return (0);
}
