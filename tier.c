/* tier.c - userspace front end for the fork_guard kernel interface.
 * Build: gcc -Wall -Wextra -O2 -o tier tier.c
 * Usage: tier set <user> <gold|silver|bronze> | tier reset | tier show
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define CTL_PATH "/proc/fork_guard_ctl"

static int send_cmd(const char *cmd)
{
	int fd = open(CTL_PATH, O_WRONLY);
	if (fd < 0) {
		perror("tier: cannot open " CTL_PATH " (root required, kernel must be -forkguard)");
		return -1;
	}
	ssize_t n = write(fd, cmd, strlen(cmd));
	int err = errno;
	close(fd);
	if (n < 0) {
		errno = err;
		perror("tier: kernel rejected command");
		return -1;
	}
	return 0;
}

static void cat_file(const char *path)
{
	char line[256];
	FILE *f = fopen(path, "r");
	if (!f) {
		perror(path);
		return;
	}
	while (fgets(line, sizeof(line), f))
		fputs(line, stdout);
	fclose(f);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: tier set <username> <gold|silver|bronze>\n"
		"       tier reset\n"
		"       tier show\n");
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "show")) {
		cat_file("/proc/sched_tier");
		printf("\n");
		cat_file("/proc/fork_guard");
		return 0;
	}

	if (argc == 2 && !strcmp(argv[1], "reset")) {
		if (send_cmd("reset\n"))
			return 1;
		puts("all users reset to SILVER");
		return 0;
	}

	if (argc == 4 && !strcmp(argv[1], "set")) {
		const char *t = argv[3];
		char cmd[64];
		struct passwd *pw;

		if (strcmp(t, "gold") && strcmp(t, "silver") && strcmp(t, "bronze")) {
			usage();
			return 1;
		}
		pw = getpwnam(argv[2]);
		if (!pw) {
			fprintf(stderr, "tier: no such user '%s'\n", argv[2]);
			return 1;
		}
		snprintf(cmd, sizeof(cmd), "set %u %s\n", (unsigned)pw->pw_uid, t);
		if (send_cmd(cmd))
			return 1;
		printf("user %s (uid %u) -> %s\n", argv[2], (unsigned)pw->pw_uid, t);
		return 0;
	}

	usage();
	return 1;
}
