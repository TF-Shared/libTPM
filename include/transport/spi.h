#ifndef SPI_TRANSPORT_H
#define SPI_TRANSPORT_H

struct spi_ops {
	void (*get_access)(void *ctx);
	void (*start)(void *ctx);
	void (*stop)(void *ctx);
	int (*xfer)(void *ctx, unsigned int bitlen,
		    const void *dout, void *din);
};

struct spi_plat {
	const struct spi_ops *ops;
};

#endif /* SPI_TRANSPORT_H */