/*
 * Copyright (c) 2025-2026, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TPM_SPI_TRANSPORT_H
#define TPM_SPI_TRANSPORT_H

struct tpm_spi_priv;

struct tpm_spi_ops {
	int (*get_access)(struct tpm_spi_priv *ctx);
	void (*release_access)(struct tpm_spi_priv *ctx);
	void (*start)(struct tpm_spi_priv *ctx);
	void (*stop)(struct tpm_spi_priv *ctx);
	int (*xfer)(struct tpm_spi_priv *ctx, unsigned int bytelen,
		    const unsigned char *dout, unsigned char *din);
};

struct tpm_spi_plat {
	struct tpm_spi_priv *priv;
	const struct tpm_spi_ops *ops;
};

#endif /* TPM_SPI_TRANSPORT_H */
