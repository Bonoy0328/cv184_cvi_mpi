/* CV184x NFS diagnostic transport, mirroring the SDK ESC packet engine.
 * Only use while this process owns the idle MIPI transmitter. Unlike the
 * original SDK, the CRC covers payload bytes only, excluding the header.
 */
#ifndef KY50_DSI_RAW_H
#define KY50_DSI_RAW_H

static volatile uint32_t *dsi_raw_regs;

static int diagnostic_clock_phase(void)
{
	if (!getenv("KY50_DIAG_NO_CLK_SHIFT"))
		return 0;
	int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0)
		return -1;
	void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x0a098000);
	close(fd);
	if (map == MAP_FAILED)
		return -1;
	volatile uint32_t *phy = map;
	uint32_t old = phy[0x9c / 4];
	phy[0x9c / 4] = old & ~0x1f000000U;
	__sync_synchronize();
	printf("Diagnostic clock phase: lane select %08x -> %08x\n", old, phy[0x9c / 4]);
	munmap(map, 4096);
	return 0;
}

static int send_payload_crc(unsigned char type, const unsigned char *data, size_t size)
{
	static const uint32_t ecc_masks[] = {
		0xf12cb7, 0xf2555b, 0x749a6d, 0xb8e38e, 0xdf03f0, 0xeffc00
	};
	unsigned char packet[128] = {type, size & 0xff, (size >> 8) & 0xff, 0};
	uint16_t crc = 0xffff;
	if (!size || size > sizeof(packet) - 6)
		return -1;
	if (!dsi_raw_regs) {
		int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
		if (fd < 0)
			return -1;
		void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x0a099000);
		close(fd);
		if (map == MAP_FAILED)
			return -1;
		dsi_raw_regs = map;
	}
	if (dsi_raw_regs[0] & 7) {
		fprintf(stderr, "Raw DSI transport requires idle mode\n");
		return -1;
	}
	uint32_t header = packet[0] | packet[1] << 8 | packet[2] << 16;
	for (size_t i = 0; i < 6; ++i)
		packet[3] |= __builtin_parity(header & ecc_masks[i]) << i;
	memcpy(packet + 4, data, size);
	for (size_t i = 0; i < size; ++i) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
	}
	packet[size + 4] = crc & 0xff;
	packet[size + 5] = crc >> 8;
	for (size_t offset = 0, total = size + 6; offset < total;) {
		size_t left = total - offset;
		/* SDK workaround: avoid a final ESC chunk of one byte. */
		size_t chunk = left <= 16 ? left : left == 17 ? 15 : 16;
		dsi_raw_regs[0x0c / 4] = 1 | ((chunk - 1) << 8) | (chunk == left ? 0x10000 : 0);
		for (size_t i = 0; i < chunk; i += 4) {
			uint32_t value = 0;
			memcpy(&value, packet + offset + i, chunk - i < 4 ? chunk - i : 4);
			dsi_raw_regs[(0x10 + i) / 4] = value;
		}
		__sync_synchronize();
		dsi_raw_regs[0] = (dsi_raw_regs[0] & ~7U) | 2U;
		struct timespec start, now;
		clock_gettime(CLOCK_MONOTONIC, &start);
		while ((dsi_raw_regs[0] & 0x30) != 0x20) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			if ((now.tv_sec - start.tv_sec) * 1000000000LL + now.tv_nsec - start.tv_nsec > 20000000) {
				fprintf(stderr, "Raw DSI ESC timeout at byte %zu\n", offset);
				return -1;
			}
		}
		/* Writing the active mode back clears it, as in dsi_clr_mode(). */
		dsi_raw_regs[0] = dsi_raw_regs[0];
		__sync_synchronize();
		offset += chunk;
	}
	return 0;
}

#endif
