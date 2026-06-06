/*
 * Copyright (c) 2025-2026, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <endian_private.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <tpm2.h>
#include <tpm2_chip.h>
#include <tpm2_private.h>

#define SINGLE_BYTE 1
#define TWO_BYTES 2
#define FOUR_BYTES 4

static struct interface_ops *interface;
struct tpm_timeout_ops tpm_timeout_ops;
int tpm_last_transport_error = 0;

/*
 * TPM Command Builder Structure
 * Provides a fluent interface for building TPM commands with automatic
 * error propagation to avoid repetitive error checking.
 */
typedef struct {
	tpm_cmd *cmd;
	enum tpm_ret_value error_state;
} tpm_cmd_builder_t;

static int tpm_update_buffer(tpm_cmd_builder_t *builder, uint32_t new_data,
			     size_t new_len);

/*
 * Initialize a TPM command builder
 * @builder: Pointer to the builder structure
 * @cmd: Pointer to the command buffer
 * @tag: TPM command tag (e.g., TPM_ST_NO_SESSIONS, TPM_ST_SESSIONS)
 * @cmd_code: TPM command code (e.g., TPM_CMD_STARTUP, TPM_CMD_PCR_EXTEND)
 */
static void tpm_cmd_init(tpm_cmd_builder_t *builder, tpm_cmd *cmd, uint16_t tag,
			 uint32_t cmd_code)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->header.tag = htobe16(tag);
	cmd->header.cmd_size = htobe32(sizeof(tpm_cmd_hdr));
	cmd->header.cmd_code = htobe32(cmd_code);

	builder->cmd = cmd;
	builder->error_state = TPM_SUCCESS;
}

/*
 * Get the final error state of the builder
 * Should be called after all add operations to check for errors
 */
static int tpm_builder_finalize(tpm_cmd_builder_t *builder)
{
	return builder->error_state;
}

static int tpm_xfer(struct tpm_chip_data *chip_data, const tpm_cmd *send,
		    tpm_cmd *receive)
{
	int ret;

	ret = interface->send(chip_data, send);
	if (ret < 0) {
		ERROR("%s: send failure %d\n", __func__, ret);
		return ret;
	}

	ret = interface->receive(chip_data, receive);
	if (ret < 0) {
		ERROR("%s: receive failure %d\n", __func__, ret);
		return ret;
	}

	return TPM_SUCCESS;
}

int tpm_get_last_transport_error(void)
{
	return tpm_last_transport_error;
}

int tpm_interface_init(const struct tpm_spi_plat *transport,
		       const struct tpm_timeout_ops *timeout_ops,
		       struct tpm_chip_data *chip_data, uint8_t locality)
{
	int err;
	tpm_spidev = transport;
	if (timeout_ops == NULL) {
#ifdef HAS_LIB_TIMER
		tpm_timeout_ops = tpm_lib_timeout_ops;
#else
		ERROR("%s: delay_ops required\n", __func__);
		return TPM_INVALID_PARAM;
#endif
	} else {
		tpm_timeout_ops = *timeout_ops;
	}
	interface = tpm_interface_getops(chip_data, locality);

	err = interface->request_access(chip_data, locality);
	if (err != 0) {
		ERROR("%s: request access failure %d\n", __func__, err);
		return err;
	}

	return interface->get_info(chip_data, locality);
}

int tpm_interface_close(struct tpm_chip_data *chip_data, uint8_t locality)
{
	return interface->release_locality(chip_data, locality);
}

static int tpm_update_buffer(tpm_cmd_builder_t *builder, uint32_t new_data,
			     size_t new_len)
{
	size_t i, j, start;
	uint32_t command_size;
	tpm_cmd *buf = builder->cmd;

	union {
		uint8_t var8;
		uint16_t var16;
		uint32_t var32;
		uint8_t array[4];
	} tpm_new_data;

	if (builder->error_state != TPM_SUCCESS) {
		return builder->error_state;
	}

	command_size = be32toh(buf->header.cmd_size);

	if (command_size + new_len > MAX_SIZE_CMDBUF) {
		ERROR("%s: buf size exceeded, increase MAX_SIZE_CMDBUF\n",
		      __func__);
		builder->error_state = TPM_INVALID_PARAM;
		return builder->error_state;
	}
	/*
	 * Subtract the cmd header size from the current command size
	 * so the data buffer is written to starting at index 0.
	 */
	start = command_size - TPM_HEADER_SIZE;

	/*
	 * The TPM, according to the TCG spec, processes data in BE byte order,
	 * in the case where the Host is LE, htobe correctly handles the byte order.
	 * When updating the buffer, keep in mind to only pass sizeof(new_data) or
	 * the variable type size for the new_len function parameter. This ensures
	 * there is only the possiblility of writing 1, 2, or 4 bytes to the buffer,
	 * and that the correct number of bytes are written to data[i].
	 */
	if (new_len == SINGLE_BYTE) {
		tpm_new_data.var8 = new_data & 0xFF;
	} else if (new_len == TWO_BYTES) {
		tpm_new_data.var16 = htobe16(new_data & 0xFFFF);
	} else if (new_len == FOUR_BYTES) {
		tpm_new_data.var32 = htobe32(new_data);
	} else {
		ERROR("%s: Invalid data length\n", __func__);
		builder->error_state = TPM_INVALID_PARAM;
		return builder->error_state;
	}

	for (i = start, j = 0; i < start + new_len; i++, j++) {
		buf->data[i] = tpm_new_data.array[j];
	}
	buf->header.cmd_size = htobe32(command_size + new_len);

	return TPM_SUCCESS;
}

typedef struct {
	const uint8_t *ptr;
	size_t remain;
	enum tpm_ret_value error_state;
} tpm_buf_iter_t;

static int iter_read_u8(tpm_buf_iter_t *iter, uint8_t *out)
{
	if (iter == NULL || out == NULL || iter->ptr == NULL) {
		ERROR("%s: NULL iterator or output\n", __func__);
		return TPM_INVALID_PARAM;
	}
	if (iter->error_state != TPM_SUCCESS) {
		return iter->error_state;
	}
	if (iter->remain < 1U) {
		iter->error_state = TPM_ERR_RESPONSE;
		return TPM_ERR_RESPONSE;
	}

	*out = iter->ptr[0];
	iter->ptr += 1U;
	iter->remain -= 1U;

	return TPM_SUCCESS;
}

static int iter_read_u16_be(tpm_buf_iter_t *iter, uint16_t *out)
{
	if (iter == NULL || out == NULL || iter->ptr == NULL) {
		ERROR("%s: NULL iterator or output\n", __func__);
		return TPM_INVALID_PARAM;
	}
	if (iter->error_state != TPM_SUCCESS) {
		return iter->error_state;
	}
	if (iter->remain < 2U) {
		iter->error_state = TPM_ERR_RESPONSE;
		return TPM_ERR_RESPONSE;
	}

	*out = (uint16_t)(((uint16_t)iter->ptr[0] << 8) |
			  (uint16_t)iter->ptr[1]);
	iter->ptr += 2U;
	iter->remain -= 2U;

	return TPM_SUCCESS;
}

static int iter_read_u32_be(tpm_buf_iter_t *iter, uint32_t *out)
{
	if (iter == NULL || out == NULL || iter->ptr == NULL) {
		ERROR("%s: NULL iterator or output\n", __func__);
		return TPM_INVALID_PARAM;
	}
	if (iter->error_state != TPM_SUCCESS) {
		return iter->error_state;
	}
	if (iter->remain < 4U) {
		iter->error_state = TPM_ERR_RESPONSE;
		return TPM_ERR_RESPONSE;
	}

	*out = ((uint32_t)iter->ptr[0] << 24) | ((uint32_t)iter->ptr[1] << 16) |
	       ((uint32_t)iter->ptr[2] << 8) | (uint32_t)iter->ptr[3];
	iter->ptr += 4U;
	iter->remain -= 4U;

	return TPM_SUCCESS;
}

static int iter_read_ptr(tpm_buf_iter_t *iter, size_t len, const uint8_t **out)
{
	if (iter == NULL || out == NULL || iter->ptr == NULL) {
		ERROR("%s: NULL iterator or output\n", __func__);
		return TPM_INVALID_PARAM;
	}
	if (iter->error_state != TPM_SUCCESS) {
		return iter->error_state;
	}
	if (iter->remain < len) {
		iter->error_state = TPM_ERR_RESPONSE;
		return TPM_ERR_RESPONSE;
	}

	*out = iter->ptr;
	iter->ptr += len;
	iter->remain -= len;

	return TPM_SUCCESS;
}

static int getcap_payload_start(const tpm_cmd *resp, uint32_t expected_cap,
				tpm_buf_iter_t *iter_out)
{
	uint32_t resp_size;
	uint32_t cap;
	uint8_t more_data;

	if ((resp == NULL) || (iter_out == NULL)) {
		return TPM_INVALID_PARAM;
	}

	resp_size = be32toh(resp->header.cmd_size);
	if ((resp_size < TPM_HEADER_SIZE) || (resp_size > sizeof(tpm_cmd))) {
		return TPM_ERR_RESPONSE;
	}

	iter_out->ptr = resp->data;
	iter_out->remain = (size_t)(resp_size - TPM_HEADER_SIZE);

	iter_read_u8(iter_out, &more_data);
	(void)more_data;

	iter_read_u32_be(iter_out, &cap);
	if (iter_out->error_state != TPM_SUCCESS) {
		return iter_out->error_state;
	}
	if (cap != expected_cap) {
		return TPM_ERR_RESPONSE;
	}

	return TPM_SUCCESS;
}

static enum tpm_ret_value getcap_raw(struct tpm_chip_data *chip,
				     uint32_t capability, uint32_t property,
				     uint32_t property_count, tpm_cmd *resp_out,
				     bool *more_data_out)
{
	tpm_cmd cmd;
	tpm_cmd_builder_t builder;
	uint32_t resp_size;
	uint32_t tpm_rc;
	uint16_t resp_tag;
	int ret;

	if ((chip == NULL) || (resp_out == NULL) || (more_data_out == NULL)) {
		return TPM_INVALID_PARAM;
	}

	memset(&cmd, 0, sizeof(cmd));
	memset(resp_out, 0, sizeof(*resp_out));

	tpm_cmd_init(&builder, &cmd, TPM_ST_NO_SESSIONS,
		     TPM_CMD_GET_CAPABILITY);

	tpm_update_buffer(&builder, capability, sizeof(uint32_t));
	tpm_update_buffer(&builder, property, sizeof(uint32_t));
	tpm_update_buffer(&builder, property_count, sizeof(uint32_t));

	ret = tpm_builder_finalize(&builder);
	if (ret < 0) {
		return ret;
	}

	ret = tpm_xfer(chip, &cmd, resp_out);
	if (ret < 0) {
		return ret;
	}

	resp_tag = be16toh(resp_out->header.tag);
	if (resp_tag != TPM_ST_NO_SESSIONS) {
		return TPM_ERR_RESPONSE;
	}

	tpm_rc = be32toh(resp_out->header.cmd_code);
	if (tpm_rc != TPM_SUCCESS) {
		return TPM_ERR_RESPONSE;
	}

	resp_size = be32toh(resp_out->header.cmd_size);
	if (resp_size < (TPM_HEADER_SIZE + 1U) || resp_size > sizeof(tpm_cmd)) {
		return TPM_ERR_RESPONSE;
	}

	*more_data_out = (resp_out->data[0] != 0U);

	return TPM_SUCCESS;
}

static enum tpm_ret_value parse_algs(const tpm_cmd *resp,
				     tpm_alg_query_t *query,
				     uint32_t *parsed_count_out,
				     uint16_t *last_alg_out)
{
	tpm_buf_iter_t iter = { 0 };
	uint32_t count = 0U;
	uint16_t alg = 0U;
	uint32_t properties = 0U;
	uint32_t parsed_count = 0U;
	int ret;

	if ((resp == NULL) || (parsed_count_out == NULL) ||
	    (last_alg_out == NULL)) {
		return TPM_INVALID_PARAM;
	}

	ret = getcap_payload_start(resp, TPM_CAP_ALGS, &iter);
	if (ret != TPM_SUCCESS) {
		return ret;
	}

	iter_read_u32_be(&iter, &count);

	for (uint32_t i = 0U; i < count; i++) {
		iter_read_u16_be(&iter, &alg);
		iter_read_u32_be(&iter, &properties);
		if (iter.error_state != TPM_SUCCESS) {
			return iter.error_state;
		}

		(void)properties;
		parsed_count++;

		for (uint32_t q = 0U; q < TPM_MAX_ALG_PROPERTIES; q++) {
			if (query[q].alg_id == TPM_ALG_NULL) {
				break;
			}
			if (query[q].alg_id == alg) {
				query[q].enabled = true;
			}
		}
	}

	if (iter.error_state != TPM_SUCCESS) {
		return iter.error_state;
	}

	*parsed_count_out = parsed_count;
	*last_alg_out = alg;

	return TPM_SUCCESS;
}

static enum tpm_ret_value
parse_algs_find(const tpm_cmd *resp, uint32_t must_set_mask,
		uint32_t must_clear_mask, tpm_alg_props_cb cb,
		tpm_alg_props_ctx_t *cb_ctx, uint32_t *parsed_count_out,
		uint16_t *last_alg_out, bool *found_out)
{
	tpm_buf_iter_t iter = { 0 };
	uint32_t count = 0U;
	uint16_t alg = 0U;
	uint32_t props = 0U;
	uint32_t parsed_count = 0U;
	int ret;

	if ((resp == NULL) || (parsed_count_out == NULL) ||
	    (last_alg_out == NULL) || (found_out == NULL)) {
		return TPM_INVALID_PARAM;
	}
	if (cb == NULL) {
		return TPM_INVALID_PARAM;
	}

	ret = getcap_payload_start(resp, TPM_CAP_ALGS, &iter);
	if (ret != TPM_SUCCESS) {
		return ret;
	}

	iter_read_u32_be(&iter, &count);

	for (uint32_t i = 0U; i < count; i++) {
		iter_read_u16_be(&iter, &alg);
		iter_read_u32_be(&iter, &props);
		if (iter.error_state != TPM_SUCCESS) {
			return iter.error_state;
		}

		parsed_count++;
		*last_alg_out = alg;

		if (((props & must_set_mask) == must_set_mask) &&
		    ((props & must_clear_mask) == 0U)) {
			if (cb(alg, props, cb_ctx)) {
				*found_out = true;
				*parsed_count_out = parsed_count;
				return TPM_SUCCESS;
			}
		}
	}

	*parsed_count_out = parsed_count;
	*found_out = false;

	if (iter.error_state != TPM_SUCCESS) {
		return iter.error_state;
	}
	return TPM_SUCCESS;
}

static enum tpm_ret_value parse_pcrs(const tpm_cmd *resp,
				     tpm_pcr_bank_query_t *query,
				     uint32_t *parsed_count_out,
				     uint16_t *last_hash_out)
{
	tpm_buf_iter_t iter = { 0 };
	uint32_t count = 0U;
	uint16_t hash = 0U;
	uint8_t sizeof_select = 0U;
	const uint8_t *pcr_select = NULL;
	uint32_t parsed_count = 0U;
	int ret;

	if ((resp == NULL) || (parsed_count_out == NULL) ||
	    (last_hash_out == NULL)) {
		return TPM_INVALID_PARAM;
	}

	ret = getcap_payload_start(resp, TPM_CAP_PCRS, &iter);
	if (ret != TPM_SUCCESS) {
		return ret;
	}

	iter_read_u32_be(&iter, &count);

	for (uint32_t i = 0U; i < count; i++) {
		iter_read_u16_be(&iter, &hash);
		iter_read_u8(&iter, &sizeof_select);
		if (iter.error_state != TPM_SUCCESS) {
			return iter.error_state;
		}
		if (sizeof_select > TPM_PCR_SELECT_SIZE) {
			return TPM_ERR_RESPONSE;
		}
		iter_read_ptr(&iter, sizeof_select, &pcr_select);

		if (iter.error_state != TPM_SUCCESS) {
			return iter.error_state;
		}
		for (uint32_t q = 0U; q < TPM_MAX_PCR_SELECTIONS; q++) {
			tpm_pcr_bank_query_t *item;

			item = &query[q];
			if (item->hash_alg == TPM_ALG_NULL) {
				break;
			}
			if (item->hash_alg == hash) {
				memset(query[q].pcr_select, 0,
				       TPM_PCR_SELECT_SIZE);
				memcpy(query[q].pcr_select, pcr_select,
				       sizeof_select);
			}
		}

		parsed_count++;
	}

	if (iter.error_state != TPM_SUCCESS) {
		return iter.error_state;
	}

	*parsed_count_out = parsed_count;
	*last_hash_out = hash;

	return TPM_SUCCESS;
}

static enum tpm_ret_value
parse_pcrs_find(const tpm_cmd *resp, tpm_pcr_bank_cb cb,
		tpm_pcr_bank_ctx_t *cb_ctx, uint32_t *parsed_count_out,
		uint16_t *last_hash_out, bool *found_out)
{
	tpm_buf_iter_t iter = { 0 };
	uint32_t count = 0U;
	uint16_t hash_id = 0U;
	uint8_t sizeof_select = 0U;
	const uint8_t *pcr_select = NULL;
	uint32_t parsed_count = 0U;
	int ret;

	if ((resp == NULL) || (parsed_count_out == NULL) ||
	    (last_hash_out == NULL) || (found_out == NULL)) {
		return TPM_INVALID_PARAM;
	}
	if (cb == NULL) {
		return TPM_INVALID_PARAM;
	}

	ret = getcap_payload_start(resp, TPM_CAP_PCRS, &iter);
	if (ret != TPM_SUCCESS) {
		return ret;
	}

	iter_read_u32_be(&iter, &count);

	for (uint32_t i = 0U; i < count; i++) {
		iter_read_u16_be(&iter, &hash_id);
		iter_read_u8(&iter, &sizeof_select);
		if (iter.error_state != TPM_SUCCESS) {
			return iter.error_state;
		}
		if (sizeof_select > TPM_PCR_SELECT_SIZE) {
			return TPM_ERR_RESPONSE;
		}
		iter_read_ptr(&iter, sizeof_select, &pcr_select);
		if (iter.error_state != TPM_SUCCESS) {
			return iter.error_state;
		}

		parsed_count++;
		*last_hash_out = hash_id;

		if (cb(hash_id, pcr_select, sizeof_select, cb_ctx)) {
			*found_out = true;
			*parsed_count_out = parsed_count;
			return TPM_SUCCESS;
		}
	}

	*parsed_count_out = parsed_count;
	*found_out = false;

	if (iter.error_state != TPM_SUCCESS) {
		return iter.error_state;
	}
	return TPM_SUCCESS;
}

int tpm_startup(struct tpm_chip_data *chip_data, uint16_t mode)
{
	tpm_cmd startup_cmd, startup_response;
	tpm_cmd_builder_t builder;
	uint32_t tpm_rc;
	int ret;

	memset(&startup_response, 0, sizeof(startup_response));

	tpm_cmd_init(&builder, &startup_cmd, TPM_ST_NO_SESSIONS,
		     TPM_CMD_STARTUP);
	tpm_update_buffer(&builder, mode, sizeof(mode));

	ret = tpm_builder_finalize(&builder);
	if (ret < 0) {
		return ret;
	}

	ret = tpm_xfer(chip_data, &startup_cmd, &startup_response);
	if (ret < 0) {
		return ret;
	}

	tpm_rc = be32toh(startup_response.header.cmd_code);
	if (tpm_rc == TPM_RC_INITIALIZE) {
		WARN("%s: TPM_RC_INITIALIZE received; TPM is already initialised\n",
		     __func__);
		return TPM_ERR_ALREADY_INIT;
	} else if (tpm_rc != TPM_RESPONSE_SUCCESS) {
		ERROR("%s: response code contains error = %X\n", __func__,
		      tpm_rc);
		return TPM_ERR_RESPONSE;
	}

	return TPM_SUCCESS;
}

int tpm_pcr_extend(struct tpm_chip_data *chip_data, uint32_t index,
		   uint16_t algorithm, const uint8_t *digest,
		   uint32_t digest_len)
{
	tpm_cmd pcr_extend_cmd, pcr_extend_response;
	tpm_cmd_builder_t builder;
	uint32_t tpm_rc;
	int ret;

	memset(&pcr_extend_response, 0, sizeof(pcr_extend_response));

	if (digest == NULL) {
		ERROR("%s: NULL digest\n", __func__);
		return TPM_INVALID_PARAM;
	}

	tpm_cmd_init(&builder, &pcr_extend_cmd, TPM_ST_SESSIONS,
		     TPM_CMD_PCR_EXTEND);

	/* handle (PCR Index)*/
	tpm_update_buffer(&builder, index, sizeof(index));

	/* authorization size , session handle, nonce size, attributes*/
	tpm_update_buffer(&builder, TPM_MIN_AUTH_SIZE, sizeof(uint32_t));
	tpm_update_buffer(&builder, TPM_RS_PW, sizeof(uint32_t));
	tpm_update_buffer(&builder, TPM_ZERO_NONCE_SIZE, sizeof(uint16_t));
	tpm_update_buffer(&builder, TPM_ATTRIBUTES_DISABLE, sizeof(uint8_t));

	/* hmac/password size */
	tpm_update_buffer(&builder, TPM_ZERO_HMAC_SIZE, sizeof(uint16_t));

	/* hashes count */
	tpm_update_buffer(&builder, TPM_SINGLE_HASH_COUNT, sizeof(uint32_t));

	/* hash algorithm */
	tpm_update_buffer(&builder, algorithm, sizeof(algorithm));

	/* digest */
	for (uint32_t i = 0; i < digest_len; i++) {
		tpm_update_buffer(&builder, digest[i], sizeof(uint8_t));
	}

	ret = tpm_builder_finalize(&builder);
	if (ret < 0) {
		return ret;
	}

	ret = tpm_xfer(chip_data, &pcr_extend_cmd, &pcr_extend_response);
	if (ret < 0) {
		return ret;
	}

	tpm_rc = be32toh(pcr_extend_response.header.cmd_code);
	if (tpm_rc != TPM_RESPONSE_SUCCESS) {
		ERROR("%s: response code contains error = %X\n", __func__,
		      tpm_rc);
		return TPM_ERR_RESPONSE;
	}

	return TPM_SUCCESS;
}

int tpm_pcr_read_single(struct tpm_chip_data *chip_data, uint32_t index,
			uint16_t algorithm, uint8_t *pcr_digest_read,
			size_t pcr_digest_read_len)
{
	tpm_cmd pcr_read_cmd, pcr_read_res;
	tpm_cmd_builder_t builder;
	uint32_t tpm_rc;
	int ret;
	size_t data_size;
	uint16_t digest_len;
	struct tpm_pcr_single_read_res tpm_read_response;

	if (index >= TPM_PCR_SELECT_SIZE * 8) {
		ERROR("%s: PCR index out of range\n", __func__);
		return TPM_INVALID_PARAM;
	}
	uint32_t pcr_bitmask = 1U << index;

	if (pcr_digest_read == NULL) {
		ERROR("%s: NULL output buffer\n", __func__);
		return TPM_INVALID_PARAM;
	}

	memset(pcr_digest_read, 0, pcr_digest_read_len);

	tpm_cmd_init(&builder, &pcr_read_cmd, TPM_ST_NO_SESSIONS,
		     TPM_CMD_PCR_READ);

	/* TPML_PCR_SELECTION (count) */
	tpm_update_buffer(&builder, 1, sizeof(uint32_t)); /* Read 1 PCR only */

	/* TPMS_PCR_SELECTION (Hash algorithm) */
	tpm_update_buffer(&builder, algorithm, sizeof(algorithm));

	/* TPMS_PCR_SELECTION (PCR_SELECT_MIN) */
	tpm_update_buffer(&builder, TPM_PCR_SELECT_SIZE, sizeof(uint8_t));

	/* TPMS_PCR_SELECTION (pcrSelect) */
	/* NOT stored in big endian */
	tpm_update_buffer(&builder, pcr_bitmask & 0xFF, sizeof(uint8_t));
	tpm_update_buffer(&builder, (pcr_bitmask >> 8) & 0xFF, sizeof(uint8_t));
	tpm_update_buffer(&builder, (pcr_bitmask >> 16) & 0xFF,
			  sizeof(uint8_t));

	ret = tpm_builder_finalize(&builder);
	if (ret < 0) {
		return ret;
	}

	ret = tpm_xfer(chip_data, &pcr_read_cmd, &pcr_read_res);
	if (ret < 0) {
		return ret;
	}

	tpm_rc = be32toh(pcr_read_res.header.cmd_code);
	if (tpm_rc != TPM_RESPONSE_SUCCESS) {
		ERROR("%s: response code contains error = %x\n", __func__,
		      tpm_rc);
		return TPM_ERR_RESPONSE;
	}

	data_size = be32toh(pcr_read_res.header.cmd_size) - TPM_HEADER_SIZE;
	if (data_size > sizeof(struct tpm_pcr_single_read_res) ||
	    data_size < offsetof(struct tpm_pcr_single_read_res, digest)) {
		ERROR("%s: Abnormal command size reported\n", __func__);
		return TPM_ERR_RESPONSE;
	}

	memcpy(&tpm_read_response, pcr_read_res.data,
	       sizeof(struct tpm_pcr_single_read_res));

	digest_len = be16toh(tpm_read_response.tpml_digest_size);
	if (digest_len > MAX_DIGEST_SIZE || digest_len > pcr_digest_read_len) {
		ERROR("%s: Buffer passed for returning digest has insufficient space\n",
		      __func__);
		return TPM_INVALID_PARAM;
	}

	if (offsetof(struct tpm_pcr_single_read_res, digest) + digest_len >
	    data_size) {
		ERROR("%s: Insufficient data read\n", __func__);
		return TPM_ERR_RESPONSE;
	}

	/* Copy digest returned from PCR read  */
	memcpy(pcr_digest_read, tpm_read_response.digest, digest_len);

	return TPM_SUCCESS;
}

enum tpm_ret_value tpm_pcr_allocate_auth_password(
	struct tpm_chip_data *chip, const uint8_t *password,
	uint16_t password_len, const tpm_pcr_allocate_bank_t *banks,
	bool *out_success, uint32_t *out_max_pcr, uint32_t *out_size_needed,
	uint32_t *out_size_available)
{
	tpm_cmd cmd;
	tpm_cmd resp;
	tpm_cmd_builder_t builder;
	tpm_buf_iter_t iter = { 0 };
	uint32_t auth_size;
	uint32_t count = 0U;
	uint32_t resp_size;
	uint16_t resp_tag;
	uint32_t tpm_rc;
	uint32_t param_size;
	uint8_t alloc_success = 0U;
	int ret;

	if ((chip == NULL) || (banks == NULL) || (out_success == NULL) ||
	    (out_max_pcr == NULL) || (out_size_needed == NULL) ||
	    (out_size_available == NULL)) {
		ERROR("%s: NULL parameter\n", __func__);
		return TPM_INVALID_PARAM;
	}
	if ((password_len > 0U) && (password == NULL)) {
		ERROR("%s: NULL password with non-zero length\n", __func__);
		return TPM_INVALID_PARAM;
	}

	for (uint32_t i = 0U; i < TPM_MAX_PCR_SELECTIONS; i++) {
		if (banks[i].hash_alg == TPM_ALG_NULL) {
			break;
		}
		count++;
	}
	if (count == 0U) {
		ERROR("%s: empty bank list\n", __func__);
		return TPM_INVALID_PARAM;
	}
	if (count >= TPM_MAX_PCR_SELECTIONS) {
		ERROR("%s: bank list missing NULL terminator\n", __func__);
		return TPM_INVALID_PARAM;
	}

	memset(&resp, 0, sizeof(resp));

	tpm_cmd_init(&builder, &cmd, TPM_ST_SESSIONS, TPM_CMD_PCR_ALLOCATE);

	/* authHandle (TPM_RH_PLATFORM) */
	tpm_update_buffer(&builder, TPM_RH_PLATFORM, sizeof(uint32_t));

	/* authorizationSize */
	auth_size = TPM_MIN_AUTH_SIZE + (uint32_t)password_len;
	tpm_update_buffer(&builder, auth_size, sizeof(uint32_t));

	/* sessionHandle */
	tpm_update_buffer(&builder, TPM_RS_PW, sizeof(uint32_t));

	/* nonce size */
	tpm_update_buffer(&builder, TPM_ZERO_NONCE_SIZE, sizeof(uint16_t));

	/* session attributes */
	tpm_update_buffer(&builder, TPM_ATTRIBUTES_DISABLE, sizeof(uint8_t));

	/* auth size */
	tpm_update_buffer(&builder, password_len, sizeof(uint16_t));

	/* auth value */
	for (uint16_t i = 0U; i < password_len; i++) {
		tpm_update_buffer(&builder, password[i], sizeof(uint8_t));
	}

	/* TPML_PCR_SELECTION count */
	tpm_update_buffer(&builder, count, sizeof(uint32_t));

	for (uint32_t i = 0U; i < count; i++) {
		tpm_update_buffer(&builder, banks[i].hash_alg,
				  sizeof(uint16_t));
		tpm_update_buffer(&builder, TPM_PCR_SELECT_SIZE,
				  sizeof(uint8_t));
		for (uint32_t j = 0U; j < TPM_PCR_SELECT_SIZE; j++) {
			tpm_update_buffer(&builder, banks[i].pcr_select[j],
					  sizeof(uint8_t));
		}
	}

	ret = tpm_builder_finalize(&builder);
	if (ret < 0) {
		return ret;
	}

	ret = tpm_xfer(chip, &cmd, &resp);
	if (ret < 0) {
		return ret;
	}

	resp_tag = be16toh(resp.header.tag);
	if (resp_tag != TPM_ST_SESSIONS) {
		return TPM_ERR_RESPONSE;
	}

	tpm_rc = be32toh(resp.header.cmd_code);
	if (tpm_rc != TPM_RESPONSE_SUCCESS) {
		return TPM_ERR_RESPONSE;
	}

	resp_size = be32toh(resp.header.cmd_size);
	if ((resp_size < (TPM_HEADER_SIZE + 17U)) ||
	    (resp_size > sizeof(tpm_cmd))) {
		return TPM_ERR_RESPONSE;
	}

	iter.ptr = resp.data;
	iter.remain = (size_t)(resp_size - TPM_HEADER_SIZE);

	/* parameterSize (skip) */
	iter_read_u32_be(&iter, &param_size);
	(void)param_size;

	iter_read_u8(&iter, &alloc_success);
	iter_read_u32_be(&iter, out_max_pcr);
	iter_read_u32_be(&iter, out_size_needed);
	iter_read_u32_be(&iter, out_size_available);

	if (iter.error_state != TPM_SUCCESS) {
		return iter.error_state;
	}

	*out_success = (alloc_success != 0U);

	return TPM_SUCCESS;
}

enum tpm_ret_value tpm_getcap_query_algs(struct tpm_chip_data *chip,
					 tpm_alg_query_t *query)
{
	uint32_t property = 0U;
	uint32_t property_count = TPM_DEFAULT_PAGE_COUNT;
	uint32_t parsed_count = 0U;
	uint16_t last_alg = 0U;
	uint16_t max_alg = 0U;
	uint32_t item_count = 0U;
	bool found_term = false;
	uint32_t pages_used = 0U;
	bool more_data = false;
	tpm_cmd resp;
	int ret;

	if ((chip == NULL) || (query == NULL)) {
		ERROR("%s: NULL chip or query\n", __func__);
		return TPM_INVALID_PARAM;
	}

	for (uint32_t i = 0U; i < TPM_MAX_ALG_PROPERTIES; i++) {
		if (query[i].alg_id == TPM_ALG_NULL) {
			found_term = true;
			break;
		}
		query[i].enabled = false;
		if (query[i].alg_id > max_alg) {
			max_alg = query[i].alg_id;
		}
		item_count++;
	}
	if (!found_term) {
		ERROR("%s: query array missing NULL terminator\n", __func__);
		return TPM_INVALID_PARAM;
	}
	if (item_count == 0U) {
		return TPM_SUCCESS;
	}

	do {
		if (pages_used >= TPM_MAX_PAGES) {
			return TPM_ERR_ITERATION_LIMIT;
		}

		ret = getcap_raw(chip, TPM_CAP_ALGS, property, property_count,
				 &resp, &more_data);
		if (ret != TPM_SUCCESS) {
			return ret;
		}

		ret = parse_algs(&resp, query, &parsed_count, &last_alg);
		if (ret != TPM_SUCCESS) {
			return ret;
		}

		if (more_data && (parsed_count == 0U)) {
			return TPM_ERR_RESPONSE;
		}
		property = (uint32_t)last_alg + 1U;

		pages_used++;
	} while (more_data && (last_alg < max_alg));

	return TPM_SUCCESS;
}

enum tpm_ret_value tpm_getcap_query_pcrs(struct tpm_chip_data *chip,
					 tpm_pcr_bank_query_t *query)
{
	uint32_t property = 0U;
	uint32_t property_count = TPM_DEFAULT_PAGE_COUNT;
	uint32_t parsed_count = 0U;
	uint16_t max_hash = 0U;
	uint16_t last_hash = 0U;
	uint32_t item_count = 0U;
	bool found_term = false;
	uint32_t pages_used = 0U;
	bool more_data = false;
	tpm_cmd resp;
	int ret;

	if ((chip == NULL) || (query == NULL)) {
		ERROR("%s: NULL chip or query\n", __func__);
		return TPM_INVALID_PARAM;
	}

	for (uint32_t i = 0U; i < TPM_MAX_PCR_SELECTIONS; i++) {
		if (query[i].hash_alg == TPM_ALG_NULL) {
			found_term = true;
			break;
		}
		memset(query[i].pcr_select, 0, TPM_PCR_SELECT_SIZE);
		if (query[i].hash_alg > max_hash) {
			max_hash = query[i].hash_alg;
		}
		item_count++;
	}

	if (!found_term) {
		ERROR("%s: query array missing NULL terminator\n", __func__);
		return TPM_INVALID_PARAM;
	}
	if (item_count == 0U) {
		return TPM_SUCCESS;
	}

	do {
		if (pages_used >= TPM_MAX_PAGES) {
			return TPM_ERR_ITERATION_LIMIT;
		}

		ret = getcap_raw(chip, TPM_CAP_PCRS, property, property_count,
				 &resp, &more_data);
		if (ret != TPM_SUCCESS) {
			return ret;
		}

		ret = parse_pcrs(&resp, query, &parsed_count, &last_hash);
		if (ret != TPM_SUCCESS) {
			return ret;
		}

		if (more_data && (parsed_count == 0U)) {
			return TPM_ERR_RESPONSE;
		}
		property = (uint32_t)last_hash + 1U;
		pages_used++;
	} while (more_data && (last_hash < max_hash));

	return TPM_SUCCESS;
}

enum tpm_ret_value tpm_has_alg(struct tpm_chip_data *chip, uint16_t alg_id,
			       bool *out_supported)
{
	tpm_alg_query_t item[2];
	int ret;

	if (out_supported == NULL) {
		ERROR("%s: NULL output pointer\n", __func__);
		return TPM_INVALID_PARAM;
	}

	item[0].alg_id = alg_id;
	item[0].enabled = false;
	item[1].alg_id = TPM_ALG_NULL;
	item[1].enabled = false;
	ret = tpm_getcap_query_algs(chip, item);
	if (ret != TPM_SUCCESS) {
		return ret;
	}

	*out_supported = item[0].enabled;

	return TPM_SUCCESS;
}

enum tpm_ret_value tpm_get_alg_props(struct tpm_chip_data *chip,
				     uint16_t alg_id, bool *out_supported,
				     uint32_t *out_props)
{
	tpm_cmd resp;
	tpm_buf_iter_t iter = { 0 };
	uint32_t count = 0U;
	uint16_t alg = TPM_ALG_NULL;
	uint32_t props = 0U;
	bool more_data = false;
	int ret;

	if ((chip == NULL) || (out_supported == NULL) || (out_props == NULL)) {
		ERROR("%s: NULL chip or output pointer\n", __func__);
		return TPM_INVALID_PARAM;
	}

	ret = getcap_raw(chip, TPM_CAP_ALGS, (uint32_t)alg_id, 1U, &resp,
			 &more_data);
	if (ret != TPM_SUCCESS) {
		return ret;
	}

	*out_supported = false;
	*out_props = 0U;

	ret = getcap_payload_start(&resp, TPM_CAP_ALGS, &iter);
	if (ret != TPM_SUCCESS) {
		return ret;
	}

	iter_read_u32_be(&iter, &count);
	iter_read_u16_be(&iter, &alg);
	iter_read_u32_be(&iter, &props);

	if (iter.error_state != TPM_SUCCESS) {
		return iter.error_state;
	}

	if (alg == alg_id) {
		*out_supported = true;
		*out_props = props;
	}

	return TPM_SUCCESS;
}

enum tpm_ret_value tpm_for_each_alg_props(struct tpm_chip_data *chip,
					  uint32_t must_set_mask,
					  uint32_t must_clear_mask,
					  tpm_alg_props_cb cb,
					  tpm_alg_props_ctx_t *ctx)
{
	uint32_t property = 0U;
	uint32_t property_count = TPM_DEFAULT_PAGE_COUNT;
	uint32_t parsed_count = 0U;
	uint16_t last_alg = 0U;
	uint32_t pages_used = 0U;
	bool more_data = false;
	bool found = false;
	tpm_cmd resp;
	int ret;

	if ((chip == NULL) || (cb == NULL)) {
		ERROR("%s: NULL chip or callback\n", __func__);
		return TPM_INVALID_PARAM;
	}

	do {
		if (pages_used >= TPM_MAX_PAGES) {
			return TPM_ERR_ITERATION_LIMIT;
		}

		ret = getcap_raw(chip, TPM_CAP_ALGS, property, property_count,
				 &resp, &more_data);
		if (ret != TPM_SUCCESS) {
			return ret;
		}

		ret = parse_algs_find(&resp, must_set_mask, must_clear_mask, cb,
				      ctx, &parsed_count, &last_alg, &found);
		if (ret != TPM_SUCCESS) {
			return ret;
		}

		if (found) {
			return TPM_SUCCESS;
		}

		if (more_data && (parsed_count == 0U)) {
			return TPM_ERR_RESPONSE;
		}

		property = (uint32_t)last_alg + 1U;
		pages_used++;
	} while (more_data);

	return TPM_SUCCESS;
}

enum tpm_ret_value tpm_for_each_pcr_bank(struct tpm_chip_data *chip,
					 tpm_pcr_bank_cb cb,
					 tpm_pcr_bank_ctx_t *ctx)
{
	uint32_t property = 0U;
	uint32_t property_count = TPM_DEFAULT_PAGE_COUNT;
	uint32_t parsed_count = 0U;
	uint16_t last_hash = 0U;
	uint32_t pages_used = 0U;
	bool more_data = false;
	bool found = false;
	tpm_cmd resp;
	int ret;

	if ((chip == NULL) || (cb == NULL)) {
		ERROR("%s: NULL chip or callback\n", __func__);
		return TPM_INVALID_PARAM;
	}

	do {
		if (pages_used >= TPM_MAX_PAGES) {
			return TPM_ERR_ITERATION_LIMIT;
		}

		ret = getcap_raw(chip, TPM_CAP_PCRS, property, property_count,
				 &resp, &more_data);
		if (ret != TPM_SUCCESS) {
			return ret;
		}

		ret = parse_pcrs_find(&resp, cb, ctx, &parsed_count, &last_hash,
				      &found);
		if (ret != TPM_SUCCESS) {
			return ret;
		}

		if (found) {
			return TPM_SUCCESS;
		}

		if (more_data && (parsed_count == 0U)) {
			return TPM_ERR_RESPONSE;
		}

		property = (uint32_t)last_hash + 1U;
		pages_used++;
	} while (more_data);

	return TPM_SUCCESS;
}
