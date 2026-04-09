/*
 * Copyright (c) 2025-2026, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TPM2_H
#define TPM2_H

#include "platform/tpm_platform.h"
#include "tpm2_chip.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#define TPM_SU_CLEAR 0x0000U
#define TPM_SU_STATE 0x0001U

#define TPM_ALG_SHA1 ((uint16_t)0x0004U)
#define TPM_ALG_SHA256 ((uint16_t)0x000BU)
#define TPM_ALG_SHA384 ((uint16_t)0x000CU)
#define TPM_ALG_NULL ((uint16_t)0x0010U)
/* 24 PCRs bit-mask with 3 bytes */
#define TPM_PCR_SELECT_SIZE ((uint8_t)0x3U)
#define TPM_PCR_BANK_FULL ((1ULL << ((TPM_PCR_SELECT_SIZE) * 8)) - 1)

/* LibTPM GetCapability limits */
#define TPM_DEFAULT_PAGE_COUNT ((uint32_t)16U)
#define TPM_MAX_PAGES ((uint32_t)64U)
#define TPM_MAX_PCR_SELECTIONS ((uint32_t)8U)
#define TPM_MAX_ALG_PROPERTIES ((uint32_t)64U)

/* Return values */
enum tpm_ret_value {
	TPM_SUCCESS = 0,
	TPM_ERR_RESPONSE = -1,
	TPM_INVALID_PARAM = -2,
	TPM_ERR_TIMEOUT = -3,
	TPM_ERR_TRANSFER = -4,
	TPM_ERR_ITERATION_LIMIT = -5,
	/* Non-fatal: the TPM was already initialized. */
	TPM_ERR_ALREADY_INIT = -6,
};

typedef struct {
	uint16_t alg_id; /* TPM_ALG_* host order */
	bool enabled; /* output */
} tpm_alg_query_t;

typedef struct {
	uint16_t hash_alg; /* TPM_ALG_* */
	uint8_t pcr_select
		[TPM_PCR_SELECT_SIZE]; /* output: bank mask, zero if absent */
} tpm_pcr_bank_query_t;

typedef struct {
	uint16_t hash_alg; /* TPM_ALG_*; TPM_ALG_NULL terminates list */
	uint8_t pcr_select[TPM_PCR_SELECT_SIZE];
} tpm_pcr_allocate_bank_t;

typedef struct {
	uint32_t flags;
	uint32_t count;
} tpm_alg_props_ctx_t;

typedef bool (*tpm_alg_props_cb)(uint16_t alg_id, uint32_t alg_props,
				 tpm_alg_props_ctx_t *ctx);

typedef struct {
	uint32_t flags;
	uint32_t count;
} tpm_pcr_bank_ctx_t;

typedef bool (*tpm_pcr_bank_cb)(uint16_t hash_alg, const uint8_t *pcr_select,
				uint8_t sizeof_select, tpm_pcr_bank_ctx_t *ctx);

int tpm_get_last_transport_error(void);

int tpm_interface_init(const struct tpm_spi_plat *transport,
		       const struct tpm_timeout_ops *timeout_ops,
		       struct tpm_chip_data *chip_data, uint8_t locality);

int tpm_interface_close(struct tpm_chip_data *chip_data, uint8_t locality);

int tpm_startup(struct tpm_chip_data *chip_data, uint16_t mode);

int tpm_pcr_extend(struct tpm_chip_data *chip_data, uint32_t index,
		   uint16_t algorithm, const uint8_t *digest,
		   uint32_t digest_len);

int tpm_pcr_read_single(struct tpm_chip_data *chip_data, uint32_t index,
			uint16_t algorithm, uint8_t *pcr_digest_read,
			size_t pcr_digest_read_len);

enum tpm_ret_value tpm_pcr_allocate_auth_password(
	struct tpm_chip_data *chip, const uint8_t *password,
	uint16_t password_len, const tpm_pcr_allocate_bank_t *banks,
	bool *out_success, uint32_t *out_max_pcr, uint32_t *out_size_needed,
	uint32_t *out_size_available);

enum tpm_ret_value tpm_getcap_query_algs(struct tpm_chip_data *chip,
					 tpm_alg_query_t *query);

enum tpm_ret_value tpm_getcap_query_pcrs(struct tpm_chip_data *chip,
					 tpm_pcr_bank_query_t *query);

enum tpm_ret_value tpm_has_alg(struct tpm_chip_data *chip, uint16_t alg_id,
			       bool *out_supported);

enum tpm_ret_value tpm_get_alg_props(struct tpm_chip_data *chip,
				     uint16_t alg_id, bool *out_supported,
				     uint32_t *out_props);

enum tpm_ret_value tpm_for_each_alg_props(struct tpm_chip_data *chip,
					  uint32_t must_set_mask,
					  uint32_t must_clear_mask,
					  tpm_alg_props_cb cb,
					  tpm_alg_props_ctx_t *ctx);

enum tpm_ret_value tpm_for_each_pcr_bank(struct tpm_chip_data *chip,
					 tpm_pcr_bank_cb cb,
					 tpm_pcr_bank_ctx_t *ctx);

#endif /* TPM2_H */
