/*
 * Copyright (c) 2025, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TPM2_H
#define TPM2_H

#include <assert.h>
#include <errno.h>
#include <stdint.h>

#include <tpm2_chip.h>

// TODO: formalise logging interface
#include <stdio.h>

#define INFO printf
#define WARN printf
#define ERROR printf


/* Return values */
enum tpm_ret_value {
	TPM_SUCCESS = 0,
	TPM_ERR_RESPONSE = -1,
	TPM_INVALID_PARAM = -2,
	TPM_ERR_TIMEOUT = -3,
	TPM_ERR_TRANSFER = -4,
};

/*
 * TPM FIFO register space address offsets
 */
#define TPM_FIFO_REG_ACCESS 0x00
#define TPM_FIFO_REG_INTR_ENABLE 0x08
#define TPM_FIFO_REG_INTR_VECTOR 0x0C
#define TPM_FIFO_REG_INTR_STS 0x10
#define TPM_FIFO_REG_INTF_CAPS 0x14
#define TPM_FIFO_REG_STATUS 0x18
#define TPM_FIFO_REG_BURST_COUNT_LO 0x19
#define TPM_FIFO_REG_BURST_COUNT_HI 0x20
#define TPM_FIFO_REG_DATA_FIFO 0x24
#define TPM_FIFO_REG_VENDID 0xF00
#define TPM_FIFO_REG_DEVID 0xF02
#define TPM_FIFO_REG_REVID 0xF04

#define TPM_ST_NO_SESSIONS 0x8001U
#define TPM_ST_SESSIONS 0x8002U

#define TPM_SU_CLEAR 0x0000U
#define TPM_SU_STATE 0x0001U

#define TPM_MIN_AUTH_SIZE 9
#define TPM_RS_PW 0x40000009
#define TPM_ZERO_NONCE_SIZE 0
#define TPM_ATTRIBUTES_DISABLE 0
#define TPM_ZERO_HMAC_SIZE 0
#define TPM_SINGLE_HASH_COUNT 1

#define TPM_CMD_STARTUP 0x0144U
#define TPM_CMD_PCR_READ 0x017EU
#define TPM_CMD_PCR_EXTEND 0x0182U

#define TPM_RESPONSE_SUCCESS 0x0000U

#define TPM_ACCESS_ACTIVE_LOCALITY 1 << 5U
#define TPM_ACCESS_VALID 1 << 7U
#define TPM_ACCESS_RELINQUISH_LOCALITY 1 << 5U
#define TPM_ACCESS_REQUEST_USE 1 << 1U
#define TPM_ACCESS_REQUEST_PENDING 1 << 2U

#define TPM_STAT_VALID 1 << 7U
#define TPM_STAT_COMMAND_READY 1 << 6U
#define TPM_STAT_GO 1 << 5U
#define TPM_STAT_AVAIL 1 << 4U
#define TPM_STAT_EXPECT 1 << 3U

#define TPM_READ_HEADER -1

#define TPM_HEADER_SIZE 10
#define MAX_SIZE_CMDBUF 256
#define MAX_CMD_DATA (MAX_SIZE_CMDBUF - TPM_HEADER_SIZE)

#pragma pack(1U)
typedef struct tpm_cmd_hdr {
	uint16_t tag;
	uint32_t cmd_size;
	uint32_t cmd_code;
} tpm_cmd_hdr;

typedef struct tpm_cmd {
	tpm_cmd_hdr header;
	uint8_t data[MAX_CMD_DATA];
} tpm_cmd;
#pragma pack()

struct tpm_timeout_ops {
	uint64_t (*timeout_init_us)(uint32_t usec);
	bool (*timeout_elapsed)(uint64_t cnt);
};

int tpm_interface_init(const struct tpm_timeout_ops *timeout_ops,
		       struct tpm_chip_data *chip_data, uint8_t locality);

int tpm_interface_close(struct tpm_chip_data *chip_data, uint8_t locality);

int tpm_startup(struct tpm_chip_data *chip_data, uint16_t mode);

int tpm_pcr_extend(struct tpm_chip_data *chip_data, uint32_t index,
		   uint16_t algorithm, const uint8_t *digest,
		   uint32_t digest_len);

#endif /* TPM2_H */
