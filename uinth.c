/*
 * uinth
 * -----
 *
 * Handle interrupts in userspace using the UIO interface of the Linux kernel.
 * For interrupts coming from GPIOs, the current GPIO value is queried, too.
 *
 * Author: Mario Kicherer (dev@kicherer.org)
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <byteswap.h>

#include <linux/gpio.h>
#include <sys/ioctl.h>

#define FLAG_GPIO_VALUE (1<<0)

struct interrupt {
	char *id;
	unsigned int uio_idx;
	
	struct pollfd *p;
	
	int flags;
	
	char *gpiochip;
	uint32_t gpionum;
	
	int gpioc_fd;
	int gpio_fd;
	
	char *cmd;
};

struct interrupt *ints = 0;
unsigned int n_ints;

struct pollfd *pfds  = 0;
unsigned int n_pfds;

char *of_id = "generic-uio,ui_pdrv";
int verbose = 0;

#define BUFSIZE 256
int read_config(char *filepath) {
	FILE *f;
	char buf[BUFSIZE], key[BUFSIZE], value[BUFSIZE], chapter[BUFSIZE];
	struct interrupt *cur;
	
	f = fopen(filepath, "r");
	if (!f) {
		fprintf(stderr, "opening \"%s\" failed: %s\n", filepath, strerror(errno));
		return -errno;
	}
	
	cur = 0;
	while (1) {
		if (feof(f)) {
			break;
		}
		
		if (!fgets(buf, sizeof(buf), f)) {
			break;
		}
		// remove newline
		buf[strlen(buf)-1] = 0;
		
		if (sscanf(buf, "#%s", value) == 1) {
			continue;
		} else
		if (sscanf(buf, "[%[^]]]", chapter) == 1) {
			n_ints += 1;
			ints = (struct interrupt*) realloc(ints, sizeof(struct interrupt)*n_ints);
			pfds = (struct pollfd*) realloc(pfds, sizeof(struct pollfd)*n_ints);
			
			cur = &ints[n_ints-1];
			cur->id = strdup(chapter);
			
			if (verbose)
				printf("chapter \"%s\"\n", chapter);
		} else
		if (sscanf(buf, "%s = %[^$]", key, value) == 2) {
			if (verbose)
				printf("setting \"%s\" = \"%s\"\n", key, value);
			if (cur) {
				if (!strcmp(key, "flags")) {
					cur->flags = atoi(value);
				} else
				if (!strcmp(key, "cmd")) {
					cur->cmd = strdup(value);
				}
			} else {
				if (!strcmp(key, "of_id")) {
					of_id = strdup(value);
				}
				if (!strcmp(key, "verbose")) {
					verbose = atoi(value);
				}
			}
		} else {
			if (verbose)
				printf("unexpected line: \"%s\"\n", buf);
		}
	}
	
	fclose(f);
	
	return 0;
}

int main(void) {
	int i, j, r, fd;
	char buf[BUFSIZE];
	ssize_t nb;
	struct interrupt *cint;
	struct gpiochip_info chip_info;
	struct gpioline_info line_info;
	
	
	verbose = getenv("UINTH_VERBOSE") != 0;
	
	if (read_config(SYSCONFDIR "uinth.cfg")) {
		fprintf(stderr, "reading config failed\n");
		return -1;
	}
	
	// load the UIOs that match the given string
	snprintf(buf, sizeof(buf), "modprobe uio_pdrv_genirq of_id=\"%s\"", of_id);
	r = system(buf); assert(r==0);
	
	// uio devices are enumerated by the kernel and we have to find out which number
	// belongs to which DT entry
	n_pfds = 0;
	for (i=0; i<n_ints; i++) {
		// read the DT name of device $i
		snprintf(buf, sizeof(buf), "/sys/class/uio/uio%d/name", i);
		fd = open(buf, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "error opening uio sysfs (%s): %s\n", buf, strerror(errno));
			return -errno;
		}
		
		memset(buf, 0, sizeof(buf));
		nb = read(fd, buf, sizeof(buf));
		if (nb < 1)
			return -1;
		buf[nb-1] = 0;
		
		close(fd);
		
		// find matching struct
		for (j=0; j<n_ints; j++) {
			cint = &ints[j];
			if (!strcmp(buf, cint->id)) {
				if (verbose)
					printf("will open uio %d for %s\n", i, cint->id);
				
				cint->uio_idx = i;
				cint->p = &pfds[n_pfds];
				n_pfds += 1;
				
				snprintf(buf, sizeof(buf), "/dev/uio%d", i);
				cint->p->fd = open(buf, O_RDWR);
				if (cint->p->fd < 0) {
					fprintf(stderr, "error opening uio dev (%s): %s\n", buf, strerror(errno));
					return -errno;
				}
				cint->p->events = POLLIN;
				
				// This is a GPIO and after an interrupt we want to get the
				// current value of the GPIO. As the kernel does not provide an
				// easy way to get the GPIO by name, we have to do some additional
				// work here.
				if ((cint->flags & FLAG_GPIO_VALUE) != 0) {
					FILE *fp;
					char buf2[BUFSIZE+64];
					
					// let the shell find the right path using wildcards
					snprintf(buf, sizeof(buf), "ls /sys/class/uio/uio%d/device/supplier*/supplier/gpiochip*/dev", i);
					fp = popen(buf, "r");
					if (fp == NULL) {
						fprintf(stderr, "could not determine gpiochip dev path: %s\n", strerror(errno));
						return -errno;
					}
					
					fgets(buf, sizeof(buf), fp);
					pclose(fp);
					buf[sizeof(buf)-1] = 0;
					buf[strlen(buf)-1] = 0;
					
					// read the resulting file as it contains the MAJOR:MINOR of our GPIO controller device file
					fp = fopen(buf, "r");
					if (fp == NULL) {
						fprintf(stderr, "could not read maj:min \"%s\": %s\n", buf, strerror(errno));
						return -errno;
					}
					while (fgets(buf, sizeof(buf), fp) != NULL) {}
					fclose(fp);
					buf[sizeof(buf)-1] = 0;
					buf[strlen(buf)-1] = 0;
					
					// open the corresponding sysfs uevent file for the MAJOR:MINOR number to get the
					// device name under /dev/
					snprintf(buf2, sizeof(buf2), "/sys/dev/char/%s/uevent", buf);
					fp = fopen(buf2, "r");
					if (fp == NULL) {
						fprintf(stderr, "could not read uevent \"%s\": %s\n", buf2, strerror(errno));
						return -errno;
					}
					
					snprintf(buf2, sizeof(buf2), "/dev/");
					while (fgets(buf, sizeof(buf), fp) != NULL) {
						if (strlen(buf) > 9 && !strncmp(buf, "DEVNAME=", 8)) {
							memcpy(buf2+5, buf+8, strlen(buf)-8-1);
							buf2[5+strlen(buf)-8-1] = 0;
							break;
						}
					}
					fclose(fp);
					
					cint->gpiochip = strdup(buf2);
					
					// Now we have the controller device but we also need the GPIO number.
					// We get the number using a custom link we added to the DT.
					snprintf(buf, sizeof(buf), "/sys/class/uio/uio%d/device/of_node/gpiopath", i);
					fp = fopen(buf, "r");
					if (fp == NULL) {
						fprintf(stderr, "could not read gpiopath \"%s\": %s\n", buf, strerror(errno));
						return -errno;
					}
					while (fgets(buf, sizeof(buf), fp) != NULL) {}
					fclose(fp);
					buf[sizeof(buf)-1] = 0;
					
					// this file contains 8 bytes, the first 4 bytes are the uint32 GPIO number, the other bytes flags
					snprintf(buf2, sizeof(buf2), "/proc/device-tree/%s/gpios", buf);
					fp = fopen(buf2, "r");
					if (fp == NULL) {
						fprintf(stderr, "could not read gpio num \"%s\": %s\n", buf, strerror(errno));
						return -errno;
					}
					while (fgets(buf, sizeof(buf), fp) != NULL) {}
					fclose(fp);
					
					cint->gpionum = __bswap_32( *(uint32_t*) buf );
					
					if (!cint->gpiochip) {
						fprintf(stderr, "could not determine gpiochip\n");
						return -errno;
					}
					
					// get a file descriptor for the GPIO pin
					{
						cint->gpioc_fd = open(cint->gpiochip, O_RDONLY);
						if (cint->gpioc_fd < 0) {
							fprintf(stderr, "open %s failed: %s", cint->gpiochip, strerror(errno));
							return -errno;
						}
						
						r = ioctl(cint->gpioc_fd, GPIO_GET_CHIPINFO_IOCTL, &chip_info);
						if (r == -1) {
							fprintf(stderr, "ioctl(GPIO_GET_CHIPINFO_IOCTL) failed: %s", strerror(errno));
							close(cint->gpioc_fd);
							return -errno;
						}
						
						if (verbose) {
							printf("Controller name: %s\n", chip_info.name);
							printf("Controller label: %s\n", chip_info.label);
							printf("#lines: %d\n", chip_info.lines);
						}
						
						line_info.line_offset = cint->gpionum;
						r = ioctl(cint->gpioc_fd, GPIO_GET_LINEINFO_IOCTL, &line_info);
						if (r == -1) {
							fprintf(stderr, "ioctl(GPIO_GET_LINEINFO_IOCTL) failed: %s", strerror(errno));
							return -errno;
						}
						
						if (verbose) {
							printf("Name: \"%s\" flags: %s%s%s%s%s\n", line_info.name,
								(line_info.flags & GPIOLINE_FLAG_IS_OUT) ? "OUTPUT" : "INPUT",
								(line_info.flags & GPIOLINE_FLAG_ACTIVE_LOW) ? " ACTIVE_LOW" : " ACTIVE_HIGH",
								(line_info.flags & GPIOLINE_FLAG_OPEN_DRAIN) ? " OPEN_DRAIN" : "",
								(line_info.flags & GPIOLINE_FLAG_OPEN_SOURCE) ? " OPENSOURCE" : "",
								(line_info.flags & GPIOLINE_FLAG_KERNEL) ? " KERNEL" : "");
						}
						
						struct gpiohandle_request rq;
						rq.lineoffsets[0] = cint->gpionum;
						rq.flags = GPIOHANDLE_REQUEST_INPUT;
						rq.lines = 1;
						r = ioctl(cint->gpioc_fd, GPIO_GET_LINEHANDLE_IOCTL, &rq);
						if (r == -1) {
							fprintf(stderr, "ioctl(GPIO_GET_LINEHANDLE_IOCTL): %s", strerror(errno));
							return -errno;
						}
						cint->gpio_fd = rq.fd;
					}
				}
				
				break;
			}
		}
	}
	
	uint32_t info = 1;
	while (1) {
		// enable interrupts
		for (j=0; j<n_ints; j++) {
			cint = &ints[j];
			
			if (!cint->p) {
				continue;
			}
			nb = write(cint->p->fd, &info, sizeof(info));
			if (nb != (ssize_t)sizeof(info)) {
				fprintf(stderr, "write umask %s %d: %s\n", cint->id, cint->p->fd, strerror(errno));
				return -errno;
			}
		}
		
		r = poll(pfds, n_pfds, -1);
		if (r >= 1) {
			// go through our list and find the ones with a event flag set
			for (j=0; j<n_ints; j++) {
				cint = &ints[j];
				if (cint->p && cint->p->revents != 0) {
					nb = read(cint->p->fd, &info, sizeof(info));
					if (nb == (ssize_t)sizeof(info)) {
						// also query the GPIO value?
						if ((cint->flags & FLAG_GPIO_VALUE) != 0) {
							struct gpiohandle_data data;
							
							r = ioctl(cint->gpio_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);
							if (r == -1) {
								fprintf(stderr, "ioctl(GPIOHANDLE_GET_LINE_VALUES_IOCTL): %s", strerror(errno));
							} else {
								if (cint->cmd) {
									snprintf(buf, sizeof(buf), cint->cmd, data.values[0]);
									system(buf);
									
									if (verbose)
										printf("int: %s GPIO value: %d\n", cint->id, data.values[0]);
								} else {
									printf("int: %s GPIO value: %d\n", cint->id, data.values[0]);
								}
							}
						} else {
							if (cint->cmd) {
								system(cint->cmd);
								
								if (verbose)
									printf("int: %s\n", cint->id);
							} else {
								printf("int: %s\n", cint->id);
							}
						}
						
						cint->p->events = POLLIN;
						cint->p->revents = 0;
					} else {
						fprintf(stderr, "unexpected amount of data from uio device: %zd != %zu\n", nb, sizeof(info));
						return -1;
					}
				}
			}
		} else {
			fprintf(stderr, "poll() returned: %s\n", strerror(r));
			return -1;
		}
	}
	
	return 0;
}
