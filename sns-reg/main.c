// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Qualcomm Sensor Registry (SNS_REG) Server
 * 
 * Copyright (C) 2023 Yassine Oudjana
 * Android port for the LG V30 (joan): reads the vendor sensor_def files.
 */

#include <errno.h>
#include <fcntl.h>
#include <libqrtr.h>
#include <sys/system_properties.h>
#include <linux/qrtr.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "map.h"
#include "qmi/sns_reg.h"

#define LINE_SIZE	0x100
#define BUFFER_LEN	0x1000

struct qmi_header {
	unsigned char type;
	unsigned short txn_id;
	unsigned short msg_id;
	unsigned short msg_len;
} __attribute__((packed));

static unsigned long registry[KEY_MAX] = { };

/*
 * Android port: read the vendor sensor_def files the proprietary sensor
 * daemon used. Items are "<id> <value> <version>"; ":property <key> <value>",
 * ":hardware <string>" and ":platform <string>" lines make the items that
 * follow conditional, and a bare metadata key resets the condition.
 * Values are parsed like the daemon did (strtoull, base 0), so negative axis
 * maps keep their two's complement low bytes.
 */
static bool platform_matches(const char *want)
{
	static const char *const paths[] = {
		"/sys/devices/soc0/hw_platform",
		"/sys/devices/soc0/platform_subtype",
	};
	char buf[64];

	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		FILE *f = fopen(paths[i], "r");

		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f)) {
			buf[strcspn(buf, "\n")] = 0;
			if (!strcmp(buf, want)) {
				fclose(f);
				return true;
			}
		}
		fclose(f);
	}
	return false;
}

static bool hardware_matches(const char *want)
{
	FILE *f = fopen("/proc/cpuinfo", "r");
	char line[LINE_SIZE];
	bool match = false;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "Hardware", 8) && strstr(line, want))
			match = true;
	fclose(f);
	return match;
}

static int load_sensor_def(const char *path)
{
	FILE *file = fopen(path, "r");
	char line[LINE_SIZE], meta[64], arg1[PROP_VALUE_MAX], arg2[PROP_VALUE_MAX];
	char prop[PROP_VALUE_MAX];
	bool active = true;
	int items = 0;

	if (!file) {
		fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), file)) {
		char *p = line + strspn(line, " \t");
		unsigned long long id, value;
		char *end;
		int n;

		if (*p == '#' || *p == '\n' || !*p)
			continue;

		if (*p == ':') {
			n = sscanf(p + 1, "%63s %91s %91s", meta, arg1, arg2);
			if (n <= 1) {
				active = true;
			} else if (!strcmp(meta, "property")) {
				__system_property_get(arg1, prop);
				active = n == 3 && !strcmp(prop, arg2);
			} else if (!strcmp(meta, "hardware")) {
				active = hardware_matches(arg1);
			} else if (!strcmp(meta, "platform")) {
				active = platform_matches(arg1);
			}
			continue;
		}

		if (!active)
			continue;

		id = strtoull(p, &end, 0);
		if (end == p)
			continue;
		p = end;
		value = strtoull(p, &end, 0);
		if (end == p)
			continue;
		if (id >= KEY_MAX) {
			fprintf(stderr, "%s: item %llu out of range\n", path, id);
			continue;
		}
		registry[id] = value;
		items++;
	}

	fclose(file);
	printf("Loaded %d items from %s\n", items, path);
	return 0;
}

static unsigned long *load_registry(void)
{
	static const char *const files[] = {
		"/vendor/etc/sensors/sensor_def_common.conf",
		"/vendor/etc/sensors/sensor_def_variable.conf",
	};

	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
		if (load_sensor_def(files[i]))
			return NULL;

	return registry;
}

int handle_group_request(int sock, struct qrtr_packet *packet, unsigned long *registry)
{
	unsigned int txn_id;
	struct sns_reg_group_req request;
	struct sns_reg_group_resp response = { };
	const struct group *group = NULL;
	size_t index = 0;
	size_t size = 0;
	struct key key;
	struct qrtr_packet out_packet;
	int ret;

	ret = qmi_decode_message(
				&request,
				&txn_id,
				packet,
				QMI_REQUEST,
				SNS_REG_GROUP_MSG_ID,
				sns_reg_group_req_ei
			);
	if (ret < 0) {
		fprintf(stderr, "Failed to decode request\n");
		return ret;
	}

	group = group_by_id(request.id);

	printf("Requested group %04d%s\n", request.id, !group ? " unmapped!" : "");

	response.id = request.id;

	if (!group) {
		response.result = QMI_RESULT_FAILURE_V01;
	} else {
		response.result = QMI_RESULT_SUCCESS_V01;

		index = 0;

		while ((key = group->keys[index++]).len) {
			if (key.id != -1)
				memcpy(response.data + size, &registry[key.id], key.len);

			size += key.len;
		}

		response.data_len = size;
	}

	out_packet.data = malloc(BUFFER_LEN);
	out_packet.data_len = BUFFER_LEN;

	ret = qmi_encode_message(
		&out_packet,
		QMI_RESPONSE,
		SNS_REG_GROUP_MSG_ID,
		txn_id,
		&response,
		sns_reg_group_resp_ei
	);
	if (ret < 0) {
		fprintf(stderr, "Failed to encode response: %d\n", ret);
		goto out;
	}

	ret = qrtr_sendto(sock, packet->node, packet->port, out_packet.data, out_packet.data_len);
	if (ret)
		fprintf(stderr, "Failed to send response: %d\n", ret);

out:
	free(out_packet.data);

	return ret;
}

int main(int argc, const char **argv)
{
	unsigned long *registry;
	int sock;
	int pollfds;
	struct sockaddr_qrtr sq;
	unsigned char buffer[BUFFER_LEN];
	struct qrtr_packet packet;
	struct qmi_header *header;
	struct sns_reg_group_req request;
	int ret;

	/* init routes stdout to the kernel log; flush per line */
	setvbuf(stdout, NULL, _IOLBF, 0);

	registry = load_registry();
	if (!registry) {
		fprintf(stderr, "Failed to parse registry\n");
		return 1;
	}

	sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (sock < 0) {
		fprintf(stderr, "Failed to create QRTR socket: %s\n", strerror(errno));
		return 1;
	}

	ret = qrtr_publish(sock, SNS_REG_QMI_SVC_ID, SNS_REG_QMI_SVC_V1, SNS_REG_QMI_INS_ID);
	if (ret) {
		fprintf(stderr, "Failed to publish service: %d\n", ret);
		return 1;
	}

	while (true) {
		pollfds = qrtr_poll(sock, -1);
		if (!pollfds)
			continue;

		ret = qrtr_recvfrom(
			sock,
			buffer,
			BUFFER_LEN,
			&sq.sq_node,
			&sq.sq_port
		);
		if (ret < 0) {
			fprintf(stderr, "Failed to receive QRTR packet\n");
			continue;
		}

		ret = qrtr_decode(&packet, buffer, BUFFER_LEN, &sq);
		if (ret < 0) {
			fprintf(stderr, "Failed to decode QRTR packet\n");
			continue;
		}

		if (packet.type != QRTR_TYPE_DATA)
			continue;

		header = (struct qmi_header *)packet.data;
		packet.data_len = sizeof(struct qmi_header) + header->msg_len;

		if (header->msg_id == SNS_REG_GROUP_MSG_ID)
			handle_group_request(sock, &packet, registry);
	}

	return 0;
}
