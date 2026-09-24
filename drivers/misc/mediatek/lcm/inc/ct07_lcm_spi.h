#ifndef __CT07_LCM_SPI_H__
#define __CT07_LCM_SPI_H__

int ct07_lcm_spi_send_cmd(unsigned int cmd);
int ct07_lcm_spi_send_rows(const unsigned char *buf, unsigned int y0,
			   unsigned int rows, unsigned int width);
int ct07_lcm_spi_send_frame(const unsigned char *buf, unsigned int len);
int ct07_lcm_spi_send_data(const unsigned char *data, unsigned int len);
void ct07_lcm_diag_stage(const char *stage);
void ct07_lcm_spi_seq_begin(void);
void ct07_lcm_spi_seq_end(void);
extern unsigned int ct07_lcm_spi_epoch;

#endif
