/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Give joan's WCN3990 a public BD address.
 *
 * The controller has no factory address (the NVM address TLV in
 * qca/crnv21.bin is zero, and LG's stack generated a random one into a
 * /persist file this device does not have), so hci_qca registers hci0 as
 * HCI_UNCONFIGURED. Unconfigured controllers are missing from the mgmt
 * index list, and the AOSP Linux HCI HAL waits for hci0 there forever.
 *
 * Derive a stable, locally administered address from the fused SoC serial
 * and set it over the mgmt control channel; the kernel then re-adds hci0
 * as a configured controller.
 */

#define LOG_TAG "joan_btaddr"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <log/log.h>

#define BTPROTO_HCI 1
#define HCI_CHANNEL_CONTROL 3
#define HCI_DEV_NONE 0xffff

#define MGMT_OP_READ_INDEX_LIST 0x0003
#define MGMT_OP_READ_UNCONF_INDEX_LIST 0x0036
#define MGMT_OP_SET_PUBLIC_ADDRESS 0x0039
#define MGMT_EV_CMD_COMPLETE 0x0001

#define SERIAL_FILE "/sys/devices/soc0/serial_number"
#define WAIT_SECS 60

struct sockaddr_hci {
	sa_family_t hci_family;
	unsigned short hci_dev;
	unsigned short hci_channel;
};

struct mgmt_hdr {
	uint16_t opcode;
	uint16_t index;
	uint16_t len;
} __attribute__((packed));

static int mgmt_cmd(int fd, uint16_t op, uint16_t index, const void *param,
		    uint16_t len, uint8_t *status, uint8_t *rsp, size_t rsp_len)
{
	uint8_t buf[512];
	struct mgmt_hdr *hdr = (struct mgmt_hdr *)buf;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	hdr->opcode = op;
	hdr->index = index;
	hdr->len = len;
	if (len)
		memcpy(buf + sizeof(*hdr), param, len);
	if (write(fd, buf, sizeof(*hdr) + len) < 0)
		return -errno;

	while (poll(&pfd, 1, 5000) > 0) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < (ssize_t)(sizeof(*hdr) + 3))
			continue;
		if (hdr->opcode != MGMT_EV_CMD_COMPLETE ||
		    (buf[6] | buf[7] << 8) != op)
			continue;
		*status = buf[8];
		if (rsp)
			memcpy(rsp, buf + 9, n - 9 < (ssize_t)rsp_len ?
			       (size_t)(n - 9) : rsp_len);
		return 0;
	}
	return -ETIMEDOUT;
}

/* Returns 1 if hci0 is in the list read by @op. */
static int has_hci0(int fd, uint16_t op)
{
	uint8_t status, rsp[2 + 2 * 16] = { 0 };
	int i, count;

	if (mgmt_cmd(fd, op, HCI_DEV_NONE, NULL, 0, &status, rsp, sizeof(rsp)) ||
	    status)
		return 0;
	count = rsp[0] | rsp[1] << 8;
	for (i = 0; i < count && i < 16; i++)
		if ((rsp[2 + 2 * i] | rsp[3 + 2 * i] << 8) == 0)
			return 1;
	return 0;
}

int main(void)
{
	struct sockaddr_hci addr = {
		.hci_family = AF_BLUETOOTH,
		.hci_dev = HCI_DEV_NONE,
		.hci_channel = HCI_CHANNEL_CONTROL,
	};
	unsigned long long serial;
	uint8_t bdaddr[6], status;
	FILE *f;
	int fd, i;

	f = fopen(SERIAL_FILE, "r");
	if (!f || fscanf(f, "%llu", &serial) != 1) {
		ALOGE("cannot read %s", SERIAL_FILE);
		return 1;
	}
	fclose(f);

	/* bdaddr_t is little endian; 02:00:<serial> is unicast, locally administered. */
	bdaddr[5] = 0x02;
	bdaddr[4] = 0x00;
	bdaddr[3] = serial >> 24;
	bdaddr[2] = serial >> 16;
	bdaddr[1] = serial >> 8;
	bdaddr[0] = serial;

	fd = socket(PF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ALOGE("cannot open the mgmt control channel: %s", strerror(errno));
		return 1;
	}

	/* hci0 appears once hci_qca has downloaded the controller firmware. */
	for (i = 0; i < WAIT_SECS; i++) {
		if (has_hci0(fd, MGMT_OP_READ_INDEX_LIST)) {
			ALOGI("hci0 is already configured");
			return 0;
		}
		if (has_hci0(fd, MGMT_OP_READ_UNCONF_INDEX_LIST))
			break;
		sleep(1);
	}
	if (i == WAIT_SECS) {
		ALOGE("hci0 did not appear within %ds", WAIT_SECS);
		return 1;
	}

	if (mgmt_cmd(fd, MGMT_OP_SET_PUBLIC_ADDRESS, 0, bdaddr, sizeof(bdaddr),
		     &status, NULL, 0) || status) {
		ALOGE("SET_PUBLIC_ADDRESS failed (status %d)", status);
		return 1;
	}

	for (i = 0; i < 10; i++) {
		if (has_hci0(fd, MGMT_OP_READ_INDEX_LIST)) {
			ALOGI("hci0 configured as %02X:%02X:%02X:%02X:%02X:%02X",
			      bdaddr[5], bdaddr[4], bdaddr[3], bdaddr[2],
			      bdaddr[1], bdaddr[0]);
			return 0;
		}
		sleep(1);
	}
	ALOGE("address accepted but hci0 did not come back configured");
	return 1;
}
