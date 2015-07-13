#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

#define MAX_SIZE	(8*1024*1024)
#define BLOCK_SIZE	16

#define u8 unsigned char
#define u32 unsigned int
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define VCT_I2C_ADDR	0x68

#define VCT_VERSION	0x03
#define VCT_READ	0x23
#define VCT_WRITE	0x22
#define VCT_ERASE	0x2a

struct vct_cmd {
	u8 cmd;
	u8 addr1;
	u8 addr2;
	u8 addr3;
	u8 count;
} __packed__;

u8 vct_get_version(int i2c) {
	if (i2c_smbus_write_byte(i2c, 0x03) < 0) {
		perror("Error sending version command");
		return 0;
	}

	int ver = i2c_smbus_read_byte(i2c);
	if (ver < 0) {
		perror("Error getting version number");
		return 0;
	}

	return ver;
}

int vct_erase(int i2c) {
	if (i2c_smbus_write_byte_data(i2c, VCT_ERASE, 0x00)) {
		perror("Error sending erase command");
		return 1;
	}

	return 0;
}

int vct_transfer(int i2c, u32 address, u8 count, void *buf, u8 cmd) {
	struct vct_cmd vct_cmd = {
		.cmd = cmd,
		.addr1 = (address & 0xff0000) >> 16,
		.addr2 = (address & 0xff00) >> 8,
		.addr3 = address & 0xff,
		.count = count
	};
	struct i2c_msg messages[] = {
		{
			.addr = VCT_I2C_ADDR,
			.flags = 0,
			.len = sizeof(vct_cmd),
			.buf = (void *)&vct_cmd
		}, {
			.addr = VCT_I2C_ADDR,
			.flags = (cmd == VCT_READ) ? I2C_M_RD : 0,
			.len = count,
			.buf = buf
		},
	};
	struct i2c_rdwr_ioctl_data msgset = {
		.msgs = messages,
		.nmsgs = ARRAY_SIZE(messages),
	};

	if (ioctl(i2c, I2C_RDWR, &msgset) < 0) {
		perror("I2C error");
		return 1;
	}

	return 0;
}

void usage() {
	printf("Usage: vct_flash BUS read|write FILE [SIZE]\n");
	printf(" BUS = I2C bus device file (/dev/i2c-N)\n");
	printf(" read  = read SIZE bytes from flash into FILE\n");
	printf(" write = write from FILE to flash (size = file size)\n");
}

int main(int argc, char *argv[]) {
	FILE *f;
	unsigned int i;
	u8 *buf;
	size_t size;
	int ret = 0;

	printf("vct_flash - Micronas VCT I2C Flash Utility\n");
	printf("Copyright (c) 2015 Ondrej Zary - http://www.rainbow-software.org\n\n");

	if (argc < 4) {
		usage();
		exit(1);
	}



	int i2c = open(argv[1], O_RDWR);
	if (i2c < 0) {
		perror("Unable to open I2C device");
		exit(2);
	}

	if (ioctl(i2c, I2C_SLAVE, VCT_I2C_ADDR) < 0) {
		perror("Error setting I2C slave address");
		exit(2);
	}

	printf("Bootloader version: 0x%02x\n", vct_get_version(i2c));

	if (!strcmp(argv[2], "write")) {
		f = fopen(argv[3], "r");
		if (!f) {
			perror("Error opening file");
			exit(5);
		}
		/* determine file size */
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (size % BLOCK_SIZE != 0 || size > MAX_SIZE) {
			fprintf(stderr, "Invalid file size: must be multiple of %d B and smaller than %d B\n", BLOCK_SIZE, MAX_SIZE);
			exit(1);
		}

		buf = malloc(size);
		if (!buf) {
			fprintf(stderr, "Memory allocation error, %d bytes required.\n", size);
			exit(1);
		}

		u32 len = fread(buf, 1, size, f);
		if (len < size) {
			fprintf(stderr, "Error reading file\n");
			exit(5);
		}

		printf("Erasing flash: ");
		if (vct_erase(i2c))
			exit(6);
		vct_get_version(i2c);
		printf("done\n");

		printf("Writing flash: ");

		for (i = 0; i < size; i += BLOCK_SIZE) {
			if (vct_transfer(i2c, i, BLOCK_SIZE, buf + i, VCT_WRITE)) {
				ret = 10;
				break;
			}
			/* print dot each 1 KB */
			if (i % 1024 == 0) {
				printf(".");
				fflush(stdout);
			}
		}
		if (i == size)
			printf("done\n");
	} else if (!strcmp(argv[2], "read")) {
		if (argc < 5) {
			fprintf(stderr, "Size not specified\n");
			exit(1);
		}
		size = atoi(argv[4]);
		if (size % BLOCK_SIZE != 0 || size > MAX_SIZE) {
			fprintf(stderr, "Invalid size specified: must be multiple of %d and smaller than %d\n", BLOCK_SIZE, MAX_SIZE);
			exit(1);
		}

		buf = malloc(size);
		if (!buf) {
			fprintf(stderr, "Memory allocation error, %d bytes required.\n", size);
			exit(1);
		}

		f = fopen(argv[3], "w");
		if (!f) {
			perror("Error opening file");
			exit(5);
		}

		printf("Reading flash: ");

		for (i = 0; i < size; i += BLOCK_SIZE) {
			if (vct_transfer(i2c, i, BLOCK_SIZE, buf + i, VCT_READ)) {
				ret = 10;
				break;
			}
			/* print dot each 1 KB */
			if (i % 1024 == 0) {
				printf(".");
				fflush(stdout);
			}
		}

		if (i == size)
			printf("done\n");
		if (fwrite(buf, 1, i, f) != i) {
			fprintf(stderr, "Error writing file\n");
			exit(5);
		}
	} else {
		usage();
		exit(1);
	}

	fclose(f);
	free(buf);
	close(i2c);

	return ret;
}
