#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int debug = 0;
int human_readable = 0;
int child_pid = -1;
int stdin_out = -1;
int stdout_in = -1;

int tid;
int interval;

void dprintf(const char *fmt, ...) {
  if (debug) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
  }
}

void dump(const void *p, size_t length) {
  const unsigned char *c = (const unsigned char *) p;
  size_t offset;
  for (offset = 0; offset < length; offset += 16) {
    size_t offset2;
    fprintf(stderr, "%04zX: ", offset);
    for (offset2 = 0; offset2 < 16 && offset + offset2 < length; offset2 ++)
      fprintf(stderr, "%02X ", c[offset + offset2]);
    fprintf(stderr, "\n");
  }
}

int plug_connected(void) {
  return child_pid != -1;
}

int plug_connect(const char *device) {
  int stdin_pipe[2] = { -1, -1 };
  int stdout_pipe[2] = { -1, -1 };
  int child;

  if (child_pid != -1 || stdin_out != -1 || stdout_in != -1) {
    return -1;
  }
  
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
    perror("pipe");
    goto bail;
  }
  child = fork();
  if (child == -1) {
    perror("fork");
    goto bail;
  }
  if (child == 0) {
    // child process
    char *child_path = "/usr/bin/rfcomm_sppd";
    char *child_argv[3] = { child_path, "-a", strdup(device) };
    close(0);
    close(1);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    if (dup2(stdin_pipe[0], 0) == -1 || dup2(stdout_pipe[1], 1) == -1) {
      perror("dup2");
      exit(1);
    }
    if (!debug) {
      close(2);
    }
    if (execv(child_path, child_argv) == -1) {
      perror("execv");
      exit(1);
    }
    perror("execv(unexpected)");
    exit(1);
  }
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);

  tid = arc4random() & 0xFFFF;
  child_pid = child;
  stdin_out = stdin_pipe[1];
  stdout_in = stdout_pipe[0];
  return 0;

bail:
  if (stdin_pipe[0] != -1)
    close(stdin_pipe[0]);
  if (stdin_pipe[1] != -1)
    close(stdin_pipe[1]);
  if (stdout_pipe[0] != -1)
    close(stdout_pipe[0]);
  if (stdout_pipe[1] != -1)
    close(stdout_pipe[1]);
  return -1;
}

void plug_disconnect(void) {
  if (stdin_out != -1) {
    close(stdin_out);
    stdin_out = -1;
  }
  if (stdout_in != -1) {
    close(stdout_in);
    stdout_in = -1;
  }
  if (child_pid != -1) {
    int status;
    if (waitpid(child_pid, &status, WNOHANG) == -1)
      perror("waitpid");
    child_pid = -1;
  }
}

ssize_t plug_read(void *buf, size_t len) {
  ssize_t result;
  int errno_save;
  dprintf("Reading %zu bytes...\n", len);
  result = read(stdout_in, buf, len);
  errno_save = errno;
  if (result == -1) {
    dprintf("Read failed.\n");
  } else if (result >= 0) {
    dprintf("Read %zu bytes.\n", len);
  }
  if (debug && len > 0) {
    dump(buf, result);
  }
  errno = errno_save;
  return result;
}

ssize_t plug_write(const void *buf, size_t len) {
  ssize_t result;
  int errno_save;
  dprintf("Writing %zu bytes...\n", len);
  if (debug && len > 0) {
    dump(buf, len);
  }
  result = write(stdin_out, buf, len);
  errno_save = errno;
  if (result == -1) {
    dprintf("Write failed.\n");
  } else if (result >= 0) {
    dprintf("Wrote %zu bytes.\n", len);
  }
  errno = errno_save;
  return result;
}

int plug_query_power_consumption(float *value) {
  unsigned char query[] = { 0x10, 0x81, 0x00, 0x00,  0x0E, 0xF0, 0x00,  0x00, 0x22, 0x00,  0x62, 0x01, 0xE2, 0x00 };
  query[2] = tid & 0xFF;
  query[3] = (tid >> 8) & 0xFF;
  tid = (tid + 1) & 0xFFFF;
  
  if (plug_write(query, sizeof(query)) != sizeof(query)) {
    perror("write");
    return -1;
  }

  while (1) {
    unsigned char response[64];
    int length;
    length = plug_read(response, 10);
    if (length < 10) {
      perror("read");
      goto outsync;
    }
    if (response[0] != 0x10 || response[1] != 0x81) {
      goto outsync;
    }
    if (response[2] != query[2] || response[3] != query[3]) {
      /* unexpected TID */
      continue;
    }
    if (response[4] != 0x00 || response[5] != 0x22 || response[6] != 0x00 || 
	response[7] != 0x0E || response[8] != 0xF0 || response[9] != 0x00) {
      /* unexpected response */
      goto outsync;
    }
    length += plug_read(&response[10], 1);
    if (length < 11) {
      perror("read");
      goto outsync;
    }
    if (response[10] == 0x72) {
      /* query success */
      length += plug_read(&response[11], 5);
      if (length < 16) {
	perror("read");
	goto outsync;
      }
      if (response[11] != 0x01 || response[12] != 0xE2 || response[13] != 0x02) {
	goto outsync;
      }
      *value = (response[14] | (response[15] << 8)) / 10.0f;
      return 0;
    } else if(response[10] == 0x52) {
      /* query failed */
      length += plug_read(&response[11], 3);
      if (length < 16) {
	perror("read");
	goto outsync;
      }
      if (response[11] != 0x01 || response[12] != 0xE2 || response[13] != 0x00) {
	goto outsync;
      }
      return -1;
    } else {
      goto outsync;
    }

  outsync:
    fprintf(stderr, "Unexpected response: ");
    dump(response, length);
    plug_disconnect();
    return -1;
  }
}

int main(int argc, const char *argv[]) {
  const char *device = NULL;
  int pos;
  float value;

  for (pos = 1; pos < argc; pos ++) {
    if (argv[pos][0] == '-') {
      if (strcmp(argv[pos], "--debug") == 0) {
	debug = 1;
      } else if (strcmp(argv[pos], "--interval") == 0) {
	if (pos + 1 == argc) {
	  fprintf(stderr, "Insufficient argument for interval.\n");
	  return 1;
	}
	interval = atoi(argv[pos + 1]);
	pos++;
      } else if (strcmp(argv[pos], "-h") == 0) {
	human_readable = 1;
      } else {
	fprintf(stderr, "Unknown option: %s", argv[pos]);
      }
    } else {
      if (device) {
	fprintf(stderr, "Too many arguments.\n");
	return 1;
      }
      device = argv[pos];
    }
  }
  if (!device) {
    fprintf(stderr, "Too few arguments.\n");
    return 1;
  }

  if (interval == 0) {
    if (plug_connect(device)) {
      return 1;
    }
    if (!plug_query_power_consumption(&value)) {
      if (human_readable) {
	printf("Power Consumption: %.1fW\n", value);
      } else {
	printf("%.1f\n", value);
      }
    }
    plug_disconnect();
  } else {
    while (1) {
      if (!plug_connected()) {
	if (!plug_connect(device)) {
	  goto retry;
	}
      }
      if (!plug_query_power_consumption(&value)) {
	if (human_readable) {
	  printf("Power Consumption: %.1fW\n", value);
	} else {
	  printf("%.1f\n", value);
	}
      } else {
	plug_disconnect();
      }

    retry:
      sleep(interval);
    }
  }
  return 0;
}
