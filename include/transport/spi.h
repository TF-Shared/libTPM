#ifndef SPI_TRANSPORT_H
#define SPI_TRANSPORT_H

struct spi_ops {
	void (*get_access)(void *ctx);
	void (*start)(void *ctx);
	void (*stop)(void *ctx);
	int (*xfer)(void *ctx, unsigned int bytelen,
		    const void *dout, void *din);
};

struct spi_priv;

struct spi_plat {
	struct spi_priv *priv;
	const struct spi_ops *ops;
};

#endif /* SPI_TRANSPORT_H */
